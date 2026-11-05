/* =============================================================================
 * 多任务冒烟测试
 *
 * 用 job.c 的 create_Task 走完整链路：
 *     源码 -> 编译 -> 装载 -> 注册 -> 调度轮转 -> 回收
 *
 * 验证点：
 *   1. 能否连续创建多个任务（每个任务独立编译、独立 region）
 *   2. 多个 VM 是否交替执行（时间片轮转，而不是一个跑完再跑下一个）
 *   3. 任务 HALT 后能否被 PS_Delete_Process 回收干净（剩余任务数归零）
 *
 * 编译（在项目根目录）：
 *   gcc -std=gnu99 -O0 -Wall \
 *       -I code -I code/os/VM -I code/os/Memory_Manager -I code/os/loader \
 *       -I code/os/Process_Scheduler -I code/PAL -I code/DEBUG -I code/compiler \
 *       -I code/cli_win \
 *       -o test_multi_task.exe \
 *       code/cli_win/test_multi_task.c \
 *       code/cli_win/job.c \
 *       code/os/Process_Scheduler/Process_Scheduler.c \
 *       code/os/loader/Loader.c \
 *       code/os/VM/VM.c code/os/VM/VM_MM.c \
 *       code/os/Memory_Manager/MM.c \
 *       code/PAL/PAL.c \
 *       code/compiler/compiler.c code/compiler/lexer.c code/compiler/parser.c \
 *       code/compiler/codegen.c code/compiler/assembler.c code/compiler/emit.c \
 *       code/compiler/error.c code/compiler/strpool.c code/compiler/comp_mem.c
 * ============================================================================= */
#include <stdio.h>
#include "job.h"
#include "Process_Scheduler.h"
#include "MM.h"
#include "PAL.h"
#include "vm.h"

#define TASK_NUM   3
#define MAX_ROUNDS 500

/* 统计调度器里存活的任务数 */
static int task_count(void)
{
    int n = 0;
    PS_Process_Priority *ppp;
    for (ppp = PS_Process_Priority_Head; ppp != NULL; ppp = ppp->next) {
        PS_Process_Thread *p;
        for (p = ppp->thread; p != NULL; p = p->next) n++;
    }
    return n;
}

/* 打印每个任务的优先级 / ID / 状态 */
static void dump_tasks(const char* tag)
{
    PS_Process_Priority *ppp;
    printf("%s\n", tag);
    for (ppp = PS_Process_Priority_Head; ppp != NULL; ppp = ppp->next) {
        PS_Process_Thread *p;
        for (p = ppp->thread; p != NULL; p = p->next) {
            printf("   prio=%u tid=%u state=%u name=%s\n",
                   ppp->priority, p->ID, p->state,
                   p->name ? p->name : "(null)");
        }
    }
}

int main(void)
{
    mm_init();
    PS_Init();

    /* 三个任务各打印一组数字，用来观察交错顺序：
     *   若顺序执行会看到  100 101 102 200 201 202 300 301 302
     *   若时间片轮转会看到 100 101 200 201 300 301 102 202 302  */
    static const char* srcs[TASK_NUM] = {
        "func main() { print(100); print(101); print(102); return 0; }",
        "func main() { print(200); print(201); print(202); return 0; }",
        "func main() { print(300); print(301); print(302); return 0; }",
    };

    printf("========== 1) 创建 %d 个任务 ==========\n", TASK_NUM);
    for (int i = 0; i < TASK_NUM; i++) {
        uint8_t rc = create_Task(srcs[i], 1, 64);
        printf("  create_Task(%d) = %u  %s\n", i, (unsigned)rc, rc == JOB_OK ? "OK" : "FAIL");
        if (rc != JOB_OK) { printf("  -> 创建失败，中止\n"); return 1; }
    }

    dump_tasks("创建后的任务表：");
    int n0 = task_count();
    printf("  任务数 = %d（期望 %d）\n", n0, TASK_NUM);
    if (n0 != TASK_NUM) { printf("  -> 任务数不符，中止\n"); return 1; }

    printf("\n========== 2) 开始调度（每轮每任务最多 %d 步）==========\n", PS_STEP);
    int rounds = 0;
    while (task_count() > 0 && rounds < MAX_ROUNDS) {
        PS_Step();
        PS_Delete_Process();
        rounds++;
    }

    printf("\n========== 3) 结果 ==========\n");
    int left = task_count();
    printf("  调度轮数 = %d\n", rounds);
    printf("  剩余任务 = %d（期望 0）\n", left);
    printf("  结论：%s\n", left == 0 ? "多任务调度与回收均正常" : "存在任务未回收");

    return left == 0 ? 0 : 1;
}
