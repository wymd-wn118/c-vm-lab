/* =============================================================================
 * CLI：命令解析 + 调度驱动
 *
 * 设计要点
 *   1. 主循环在 CLI 自己手里（不用 PS_Main 的死循环），这样每轮读完命令
 *      都能回到这里，将来换成非阻塞输入时结构不用改。
 *   2. 调度器只提供原子能力：PS_Step() / PS_Delete_Process()。
 *   3. 两条运行路径：
 *        run    <file>  -> 读源码文本 -> create_Task（内部编译 + 装载 + 注册）
 *        runimg <file>  -> 读 .osimg 二进制 -> loader_load_mem + PS_Creat_Process_Ass
 *      runimg 之所以在 CLI 内部实现，是因为 job.c 目前只暴露源码入口。
 *   4. 输入全部走 PAL（pal_gets / pal_input_ready / pal_getch_now），
 *      CLI 不含任何平台相关代码。
 * ============================================================================= */
#include <stdio.h>          /* fflush(stdout) */

#include "cli.h"
#include "job.h"
#include "Process_Scheduler.h"
#include "MM.h"
#include "PAL.h"
#include "Loader.h"
#include "vm.h"

/* Temporary prompt with explicit flush: pal_printf is line-buffered, so a
 * trailing "> " with no newline would otherwise stay invisible until the
 * next output. Replace with pal_flush() once PAL provides one. */
static void cli_prompt(const char* s)
{
    pal_printf("%s", s);
    fflush(stdout);
}

/* ===================== 小工具 ===================== */

/* 把 p 的首个单词原地截断，返回剩余部分（已跳过空白）；无剩余返回 NULL */
static char* next_token(char* p)
{
    char* q;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0) return NULL;

    q = p;
    while (*q && *q != ' ' && *q != '\t') q++;
    if (*q == 0) return NULL;           /* 只有一个词，没有参数 */

    *q = 0;                             /* 截断命令词 */
    q++;
    while (*q == ' ' || *q == '\t') q++;
    return (*q) ? q : NULL;
}

static const char* state_name(uint8_t s)
{
    switch (s) {
    case PS_STATE_RUNNED: return "RUNNED";
    case PS_STATE_READY:  return "READY";
    case PS_STATE_BLOCK:  return "BLOCK";
    case PS_STATE_DEAD:   return "DEAD";
    default:              return "?";
    }
}

/* 统计调度器里存活的任务数 */
static int task_count(void)
{
    int n = 0;
    PS_Process_Priority* ppp;
    for (ppp = PS_Process_Priority_Head; ppp != NULL; ppp = ppp->next) {
        PS_Process_Thread* p;
        for (p = ppp->thread; p != NULL; p = p->next) n++;
    }
    return n;
}

/* 按 tid 找任务节点 */
static PS_Process_Thread* find_task(uint32_t tid)
{
    PS_Process_Priority* ppp;
    for (ppp = PS_Process_Priority_Head; ppp != NULL; ppp = ppp->next) {
        PS_Process_Thread* p;
        for (p = ppp->thread; p != NULL; p = p->next) {
            if (p->ID == tid) return p;
        }
    }
    return NULL;
}

/* 推进调度：最多 max_rounds 轮，任务全部结束时提前返回 */
static void drive_scheduler(uint32_t max_rounds)
{
    uint32_t i;
    for (i = 0; i < max_rounds; i++) {
        PS_Step();
        PS_Delete_Process();
        if (task_count() == 0) break;
    }
}

/* 推进调度直到没有任务为止（run / runtext / runimg 使用）。
 *
 * 有意不设轮数上限：限制轮数会让"长时间运行"和"死循环"这类程序永远跑不完整，
 * 而后者正是多任务存在的意义。
 *
 * 每轮用 pal_input_ready() 非阻塞检查一次：用户按任意键即打断，
 * 消费掉该按键后返回 1，让主循环回到提示符。任务仍留在调度器里，
 * 可继续用 list / step / kill 操作 —— 不需要 Ctrl+C 杀掉整个进程。
 *
 * @return 1 = 被用户按键打断（任务可能还在）；0 = 所有任务已自然结束 */
static int drive_until_empty(void)
{
    while (task_count() > 0) {
        PS_Step();
        PS_Delete_Process();
        if (pal_input_ready()) {
            (void)pal_getch_now();       /* 消费掉打断用的按键，避免污染下一条命令 */
            return 1;
        }
    }
    return 0;
}

/* ===================== 命令实现 ===================== */

static void print_help(void)
{
    pal_printf(
        "可用命令：\n"
        "  run <file>      读取源码文件，编译并运行\n"
        "  runtext         逐行输入源码，单独一行 '.' 结束\n"
        "  runimg <file>   直接装载 .osimg 镜像运行（跳过编译）\n"
        "  spawn <file>    只创建任务不运行（配合 list / step / kill）\n"
        "  spawntext       逐行输入源码，只创建不运行\n"
        "  list            列出所有任务\n"
        "  step [n]        手动推进 n 轮调度（默认 1）\n"
        "  kill <tid>      结束并回收指定任务\n"
        "  mem             打印各任务的内存占用\n"
        "  help            显示本帮助\n"
        "  quit / exit     退出\n");
}

/* From a source file: compile -> load -> register.
 * run_now=1: drive the scheduler until every task finishes (run / r**).
 * run_now=0: leave the task READY so the user can inspect it with
 *            list / step / kill (spawn). */
static int do_create_from_file(const char* path, int run_now)
{
    uint32_t sz  = 0;
    char*    src;
    uint8_t  rc;

    if (!path) { pal_printf("用法: %s <源文件>\n", run_now ? "run" : "spawn"); return -1; }

    src = pal_read_file(path, &sz);                 /* 标准 char*，直接当源码文本 */
    if (!src) { pal_printf("读取失败: %s\n", path); return -1; }

    rc = create_Task(src, 1, 64);                   /* 内部完成编译 + 装载 + 注册 */
    pal_free(src);                                  /* 编译完即无用 */

    if (rc != JOB_OK) { pal_printf("创建任务失败 rc=%u\n", rc); return -1; }

    if (run_now) {
        pal_printf("OK: 任务已创建，开始运行\n");
        if (drive_until_empty())
            pal_printf("[已中断] 任务仍在运行，可用 step 继续 / kill <tid> 结束\n");
    } else {
        pal_printf("OK: 任务已创建但未运行（用 list 查看，step 推进）\n");
    }
    return 0;
}

static int cmd_run_file(const char* path)   { return do_create_from_file(path, 1); }
static int cmd_spawn_file(const char* path) { return do_create_from_file(path, 0); }

/* Multi-line source input. run_now selects run vs spawn behaviour. */
static int do_create_from_text(int run_now)
{
    static char src[CLI_SRC_MAX];
    uint32_t used = 0;
    char line[CLI_LINE_MAX];
    uint8_t rc;

    pal_printf("逐行输入源码，单独一行 '.' 结束：\n");
    src[0] = 0;

    while (1) {
        uint32_t n, i;
        cli_prompt("| ");
        if (!pal_gets(line, sizeof line)) { pal_printf("\n已取消\n"); return -1; }
        if (pal_strcmp(line, ".") == 0) break;

        n = pal_strlen(line);
        if (used + n + 2u >= CLI_SRC_MAX) { pal_printf("源码过长，已截断\n"); break; }
        for (i = 0; i < n; i++) src[used++] = line[i];
        src[used++] = '\n';                          /* 补回换行，保证行分隔 */
        src[used]   = 0;
    }

    if (used == 0) { pal_printf("未输入内容\n"); return -1; }

    rc = create_Task(src, 1, 64);
    if (rc != JOB_OK) { pal_printf("编译/创建失败 rc=%u\n", rc); return -1; }

    if (run_now) {
        pal_printf("OK: 任务已创建，开始运行\n");
        if (drive_until_empty())
            pal_printf("[已中断] 任务仍在运行，可用 step 继续 / kill <tid> 结束\n");
    } else {
        pal_printf("OK: 任务已创建但未运行（用 list 查看，step 推进）\n");
    }
    return 0;
}

static int cmd_run_text(void)  { return do_create_from_text(1); }
static int cmd_spawntext(void) { return do_create_from_text(0); }

/* runimg：直接装载 .osimg 镜像。
 * 这里在 CLI 内部完成「分配 task_id -> 分配 vm -> 装载 -> 注册」，
 * 因为 job.c 目前只有源码入口。两条路径共用 job.h 里的 task_id_table。 */
static int cmd_run_image(const char* path)
{
    uint32_t sz  = 0;
    uint32_t tid = 0;
    uint32_t vm_off;
    vm_t*    vm;
    char*    raw;
    uint32_t i;

    if (!path) { pal_printf("用法: runimg <镜像文件>\n"); return -1; }

    raw = pal_read_file(path, &sz);                 /* 返回 char*，装载时再转 uint8_t* */
    if (!raw) { pal_printf("读取失败: %s\n", path); return -1; }

    /* ① 分配 task_id（与 job.c 共用同一张位图） */
    for (i = 0; i < JOB_TASK_SLOTS; i++) {
        if (task_id_table[i] == 0) {
            task_id_table[i] = 1;
            tid = i + JOB_TASK_ID_BASE;
            break;
        }
    }
    if (tid == 0) { pal_free(raw); pal_printf("任务槽位已满（最多 %d 个）\n", JOB_TASK_SLOTS); return -1; }

    /* ② 分配 vm 本体 */
    vm_off = mm_alloc(tid, (uint32_t)sizeof(vm_t));
    if (vm_off == MM_FAIL) {
        task_id_table[tid - JOB_TASK_ID_BASE] = 0;
        pal_free(raw);
        pal_printf("内存分配失败\n");
        return -1;
    }
    vm = (vm_t*)(mm_ram() + vm_off);

    /* ③ 装载镜像（内部会 mm_alloc 三块窗口） */
    if (loader_load_mem(vm, tid, 1, (uint8_t*)raw, sz, 64) != 0) {
        mm_reap(tid);                               /* 连已申请的窗口一起回滚 */
        task_id_table[tid - JOB_TASK_ID_BASE] = 0;
        pal_free(raw);
        pal_printf("装载失败（镜像格式不对？）\n");
        return -1;
    }
    pal_free(raw);                                  /* 内容已拷进 region */

    /* ④ 注册到调度器 */
    if (PS_Creat_Process_Ass(vm, "img", PS_PRIORITY_DEFAULT, tid) != 0) {
        mm_reap(tid);
        task_id_table[tid - JOB_TASK_ID_BASE] = 0;
        pal_printf("注册失败\n");
        return -1;
    }

    pal_printf("OK: tid=%u 已装载，开始运行\n", tid);
    if (drive_until_empty())
        pal_printf("[已中断] 任务仍在运行，可用 step 继续 / kill <tid> 结束\n");
    return 0;
}

/* list：列出所有任务 */
static int cmd_list(void)
{
    int n = 0;
    PS_Process_Priority* ppp;

    pal_printf("%-5s %-5s %-8s %s\n", "tid", "prio", "state", "name");
    for (ppp = PS_Process_Priority_Head; ppp != NULL; ppp = ppp->next) {
        PS_Process_Thread* p;
        for (p = ppp->thread; p != NULL; p = p->next) {
            pal_printf("%-5u %-5u %-8s %s\n",
                       p->ID, ppp->priority, state_name(p->state),
                       p->name ? p->name : "-");
            n++;
        }
    }
    if (n == 0) pal_printf("(无任务)\n");
    else        pal_printf("共 %d 个任务\n", n);
    return 0;
}

/* step [n]：手动推进调度 */
static int cmd_step(const char* arg)
{
    uint32_t n = arg ? (uint32_t)pal_strtoul(arg, NULL, 10) : 1u;
    if (n == 0) n = 1;

    drive_scheduler(n);
    pal_printf("已推进 %u 轮，剩余任务 %d\n", n, task_count());
    return 0;
}

/* kill <tid>：标记 DEAD 后交给调度器回收（走统一回收路径） */
static int cmd_kill(const char* arg)
{
    uint32_t tid;
    PS_Process_Thread* p;

    if (!arg) { pal_printf("用法: kill <tid>\n"); return -1; }
    tid = (uint32_t)pal_strtoul(arg, NULL, 10);

    p = find_task(tid);
    if (!p) { pal_printf("找不到 tid=%u\n", tid); return -1; }

    p->state = PS_STATE_DEAD;                       /* 下一轮 PS_Delete_Process 会回收 */
    PS_Delete_Process();
    pal_printf("已回收 tid=%u\n", tid);
    return 0;
}

/* mem：打印各任务窗口占用 */
static int cmd_mem(void)
{
    int n = 0;
    PS_Process_Priority* ppp;

    for (ppp = PS_Process_Priority_Head; ppp != NULL; ppp = ppp->next) {
        PS_Process_Thread* p;
        for (p = ppp->thread; p != NULL; p = p->next) {
            vm_t* vm = (vm_t*)p->body;
            if (!vm) continue;
            pal_printf("tid=%-3u prog=%-5u data=%-5u stack=%-5u 合计=%u B\n",
                       p->ID, vm->prog.size, vm->data.size, vm->stack.size,
                       vm->prog.size + vm->data.size + vm->stack.size);
            n++;
        }
    }
    if (n == 0) pal_printf("(无任务)\n");
    return 0;
}

/* ===================== 命令分发 ===================== */

/* @return <0 表示退出主循环 */
static int dispatch(char* line)
{
    char* arg;

    if (pal_strlen(line) == 0) return 0;            /* 空行忽略 */

    /* 无参数命令 */
    if (pal_strcmp(line, "quit") == 0) return -1;
    if (pal_strcmp(line, "exit") == 0) return -1;
    if (pal_strcmp(line, "help") == 0) { print_help(); return 0; }
    if (pal_strcmp(line, "list") == 0) { cmd_list();   return 0; }
    if (pal_strcmp(line, "ls")   == 0) { cmd_list();   return 0; }
    if (pal_strcmp(line, "mem")  == 0) { cmd_mem();    return 0; }
    if (pal_strcmp(line, "runtext")  == 0) { cmd_run_text();  return 0; }
    if (pal_strcmp(line, "spawntext") == 0) { cmd_spawntext(); return 0; }

    /* 带参数命令：先把首个词切出来 */
    arg = next_token(line);
    if (pal_strcmp(line, "run")   == 0) { cmd_run_file(arg);   return 0; }
    if (pal_strcmp(line, "spawn") == 0) { cmd_spawn_file(arg); return 0; }
    if (pal_strcmp(line, "runimg") == 0) { cmd_run_image(arg); return 0; }
    if (pal_strcmp(line, "step")   == 0) { cmd_step(arg);      return 0; }
    if (pal_strcmp(line, "kill")   == 0) { cmd_kill(arg);      return 0; }

    pal_printf("未知命令: %s（输入 help 查看）\n", line);
    return 0;
}

/* ===================== 主循环 ===================== */

int cli_run(void)
{
    char line[CLI_LINE_MAX];

    /* 控制台切到 UTF-8：源码与字符串字面量都是 UTF-8，而中文 Windows 控制台
     * 默认 936(GBK)。具体怎么做由 PAL 的平台实现决定，这里无需 #if。 */
    pal_console_utf8();

    mm_init();
    PS_Init();

    /* 注册回收回调：任务被调度器回收后，job 层借此归还 task_id 槽位。
     * create_Task 内部也会注册一次，这里覆盖 runimg 这类绕过 create_Task 的路径。 */
    PS_SetReapCallback(job_release_id);

    pal_printf("OS-VM CLI 已启动，输入 help 查看命令\n");

    while (1) {
        cli_prompt("> ");
        if (!pal_gets(line, sizeof line)) break;    /* EOF */
        if (dispatch(line) < 0) break;
    }

    pal_printf("bye\n");
    pal_console_restore();                      /* 恢复原代码页，不污染同窗口的后续程序 */
    return 0;
}

int main(void)
{
    return cli_run();
}
