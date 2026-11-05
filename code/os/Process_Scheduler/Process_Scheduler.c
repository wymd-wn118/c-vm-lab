#include "Process_Scheduler.h"
#include "PAL.h"
#include "vm.h"
#include "MM.h"

/* 优先级链表头：定义在此，声明在 Process_Scheduler.h */
PS_Process_Priority *PS_Process_Priority_Head = NULL;

/* 任务回收回调：上层注册，NULL 表示没有注册。
 * 调度器只持有地址，不关心它是谁 —— 这样不需要 #include "job.h"。 */
static ps_reap_cb_t g_reap_cb = NULL;

void PS_SetReapCallback(ps_reap_cb_t cb) { g_reap_cb = cb; }

/* ===================== 内部函数前向声明 ===================== */
static int  ps_step_prio(PS_Process_Priority *ppp);
static void ps_reap_dead(PS_Process_Priority *ppp);

/* ===================== 初始化 ===================== */
int PS_Init(void)
{
    /* 幂等：已经初始化过就直接返回，避免重复调用泄漏 10 个节点 */
    if (PS_Process_Priority_Head != NULL) return 0;

    uint32_t head_off = mm_alloc(MM_TASK_SYSTEM, sizeof(PS_Process_Priority));
    if (head_off == MM_FAIL) return -1;

    PS_Process_Priority_Head = (PS_Process_Priority *)(mm_ram() + head_off);
    pal_memset(PS_Process_Priority_Head, 0, sizeof(PS_Process_Priority));  /* mm_alloc 不清零 */
    PS_Process_Priority_Head->priority = 0;

    /* 依次建立 priority = 1 .. PS_MAX_PRIORITY-1 的节点 */
    PS_Process_Priority *p = PS_Process_Priority_Head;
    for (int i = 1; i < PS_MAX_PRIORITY; i++) {
        uint32_t off = mm_alloc(MM_TASK_SYSTEM, sizeof(PS_Process_Priority));
        if (off == MM_FAIL) return -1;

        p->next = (PS_Process_Priority *)(mm_ram() + off);
        pal_memset(p->next, 0, sizeof(PS_Process_Priority));
        p->next->priority = i;
        p = p->next;
    }
    return 0;
}

/* ===================== 创建任务 ===================== */
int PS_Creat_Process_Ass(void *vm, const char *name, uint8_t priority, uint32_t task_id)
{
    if (PS_Process_Priority_Head == NULL) return -1;   /* 尚未 PS_Init */
    if (task_id < 3u)                     return -1;   /* 0/1/2 是 MM 保留号，不能作为任务 ID */
    if (priority >= PS_MAX_PRIORITY)      return -1;

    /* 找到 priority 对应的优先级节点 */
    PS_Process_Priority *PPP = PS_Process_Priority_Head;
    while (PPP->next != NULL && PPP->priority != priority) {
        PPP = PPP->next;
    }
    if (PPP->priority != priority) return -1;          /* 防御：链表结构异常 */

    /* 分配线程节点 */
    uint32_t off = mm_alloc(MM_TASK_SYSTEM, sizeof(PS_Process_Thread));
    if (off == MM_FAIL) return -1;

    PS_Process_Thread *node = (PS_Process_Thread *)(mm_ram() + off);
    pal_memset(node, 0, sizeof(PS_Process_Thread));    /* mm_alloc 不清零 */

    /* 挂到该优先级线程链表的尾部 */
    if (PPP->thread == NULL) {
        PPP->thread = node;
    } else {
        PS_Process_Thread *tail = PPP->thread;
        while (tail->next != NULL) tail = tail->next;
        tail->next = node;
    }

    node->state      = PS_STATE_READY;
    node->name       = name;
    node->Step_Slice = 0;
    node->body       = vm;
    node->ID         = task_id;
    node->next       = NULL;
    return 0;
}

/* ===================== 回收 DEAD 任务 ===================== */
static void ps_reap_dead(PS_Process_Priority *ppp)
{
    while (ppp != NULL) {
        PS_Process_Thread *prev = NULL;
        PS_Process_Thread *p    = ppp->thread;

        while (p != NULL) {
            if (p->state == PS_STATE_DEAD) {
                PS_Process_Thread *dead = p;
                p = p->next;

                /* 摘链：前驱指向后继；删的是头节点则直接更新链表头 */
                if (prev) prev->next = p;
                else      ppp->thread = p;

                /* 先回收该任务名下的 vm 与 region，再释放节点自身。
                 * 回收完再回调上层（job 层借此归还 task_id 槽位）。
                 * 注意顺序：回调可能重新创建任务、复用同一 task_id，
                 * 所以必须等 mm_reap 彻底做完再通知。 */
                if (dead->ID >= 3u) {
                    mm_reap(dead->ID);
                    if (g_reap_cb) g_reap_cb(dead->ID);
                }
                mm_free((uint32_t)((uint8_t *)dead - mm_ram()));
            } else {
                prev = p;
                p    = p->next;
            }
        }
        ppp = ppp->next;
    }
}

int PS_Delete_Process(void)
{
    if (PS_Process_Priority_Head == NULL) return -1;
    ps_reap_dead(PS_Process_Priority_Head);
    return 0;
}

/* ===================== 调度 ===================== */
static int ps_step_prio(PS_Process_Priority *ppp)
{
    if (ppp == NULL) return -1;

    int isrun = 0;

    /* 跳过没有线程的优先级 */
    while (ppp->next != NULL && ppp->thread == NULL) {
        ppp = ppp->next;
    }

    if (ppp->thread != NULL) {
        PS_Process_Thread *p = ppp->thread;
        while (p != NULL) {
            if (p->state != PS_STATE_READY) {
                p = p->next;
                continue;
            }

            isrun = 1;
            uint8_t rc = 0;                            /* 0 = VM_OK */
            for (int i = 0; i < PS_STEP; i++) {
                rc = vm_step((vm_t *)p->body);
                if (rc != 0) break;                    /* HALT 或出错，立刻停 */
            }
            if (rc != 0) {
                p->state = PS_STATE_DEAD;              /* 交给 PS_Delete_Process 回收 */
            }
            p = p->next;
        }

        /* 本优先级没有任何可运行任务，交给下一优先级 */
        if (isrun == 0 && ppp->next != NULL) {
            ps_step_prio(ppp->next);
        }
    }
    return 0;
}

int PS_Step(void)
{
    if (PS_Process_Priority_Head == NULL) return -1;
    return ps_step_prio(PS_Process_Priority_Head);
}

/* ===================== 主循环 ===================== */
int PS_Main(void)
{
    while (1) {
        PS_Step();            /* 推进一轮：每个 READY 任务跑至多 PS_STEP 步 */
        PS_Delete_Process();  /* 回收本轮结束的任务：摘链 + reap + free */
    }
    return 0;                 /* 不可达 */
}
