/******************************************************************************
 * 文件名:   WIN_ctx.c
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-10-29
 * 描述:     Windows 平台的协作式执行上下文（协程）实现。
 *          用 Windows Fiber 承载「独立栈 + 换栈」：
 *            CreateFiber  —— 为任务分配一段独立栈，首次 SwitchToFiber 时进入入口
 *            SwitchToFiber —— 保存当前现场（寄存器 + SP）并恢复目标现场
 *          语义与调度器约定完全对应：
 *            pal_ctx_resume(c)  = 从 primary 切到 c
 *            pal_ctx_yield()    = 从任务切回 primary
 *          注意：本目录（PAL/windows/）豁免「仅 C99」约束，可直接用平台 API。
 * 版本:     0.1.0
 * 版权:     无
 ******************************************************************************/
#include "PAL.h"
#include <windows.h>

/* 一个上下文 = 一段可被挂起 / 恢复的执行流 */
struct pal_ctx {
    void         *fiber;       /* Fiber 句柄（CreateFiber / ConvertThreadToFiber 的返回值） */
    pal_ctx_entry entry;       /* 任务入口 */
    void         *arg;         /* 传给入口的参数 */
    int           finished;    /* 入口函数是否已返回 */
    int           is_primary;  /* 1 = 调度器自身的上下文，不允许销毁 */
};

static pal_ctx_t *g_ctx_primary;   /* 调度器上下文，全局唯一 */

/* Fiber 入口：用户函数整个跑完才走到这里（期间可多次 yield） */
static VOID CALLBACK ps_ctx_tramp(void *param)
{
    pal_ctx_t *c = (pal_ctx_t *)param;

    c->entry(c->arg);
    c->finished = 1;
    SwitchToFiber(g_ctx_primary->fiber);

    /* 兜底：正常流程下本上下文此后只会被 DeleteFiber，不会回到这一行。
     * 若真被误 resume，就停在切回 primary 的循环里，而不是跑出函数末尾。 */
    for (;;) {
        SwitchToFiber(g_ctx_primary->fiber);
    }
}

pal_ctx_t *pal_ctx_primary(void)
{
    void      *fiber;
    pal_ctx_t *c;

    if (g_ctx_primary != NULL) return g_ctx_primary;   /* 幂等 */

    c = (pal_ctx_t *)pal_calloc(1, sizeof(pal_ctx_t));
    if (c == NULL) return NULL;

    /* 把当前线程升级为 fiber，返回的句柄就是「调度器上下文」 */
    fiber = ConvertThreadToFiber(NULL);
    if (fiber == NULL) {
        if (GetLastError() == ERROR_ALREADY_FIBER) {
            /* 已被别的模块转换过：拿不回句柄，属于调用方错误，明确失败而不是猜 */
            pal_printf("[PAL][ERR] pal_ctx_primary: 当前线程已是 fiber，无法取得主上下文句柄\n");
        } else {
            pal_printf("[PAL][ERR] pal_ctx_primary: ConvertThreadToFiber 失败 err=%lu\n",
                       (unsigned long)GetLastError());
        }
        pal_free(c);
        return NULL;
    }

    c->fiber      = fiber;
    c->is_primary = 1;
    g_ctx_primary = c;
    return c;
}

pal_ctx_t *pal_ctx_create(pal_ctx_entry entry, void *arg, uint32_t stack_bytes)
{
    pal_ctx_t *c;

    if (entry == NULL) return NULL;
    if (g_ctx_primary == NULL) return NULL;   /* 必须先有调度器上下文，否则切不回来 */

    c = (pal_ctx_t *)pal_calloc(1, sizeof(pal_ctx_t));
    if (c == NULL) return NULL;

    c->entry = entry;
    c->arg   = arg;
    c->fiber = CreateFiber((SIZE_T)stack_bytes, ps_ctx_tramp, c);
    if (c->fiber == NULL) {
        pal_printf("[PAL][ERR] pal_ctx_create: CreateFiber(%lu) 失败 err=%lu\n",
                   (unsigned long)stack_bytes, (unsigned long)GetLastError());
        pal_free(c);
        return NULL;
    }
    return c;
}

void pal_ctx_resume(pal_ctx_t *c)
{
    if (c == NULL || c->fiber == NULL || c->finished) return;   /* 已结束的不许再进 */
    SwitchToFiber(c->fiber);                                    /* 任务 yield 或结束时才返回 */
}

void pal_ctx_yield(void)
{
    if (g_ctx_primary == NULL) return;
    SwitchToFiber(g_ctx_primary->fiber);
}

int pal_ctx_finished(pal_ctx_t *c)
{
    return (c != NULL) ? c->finished : 1;
}

void pal_ctx_destroy(pal_ctx_t *c)
{
    if (c == NULL || c->is_primary) return;   /* 调度器上下文随进程退出，不单独销毁 */

    if (c->fiber != NULL) {
        /* DeleteFiber 会释放该 fiber 的栈；只能对「非当前运行中」的 fiber 调用，
         * 调度器在任务结束（tramp 已切回 primary）之后才回收，因此是安全的。 */
        DeleteFiber(c->fiber);
        c->fiber = NULL;
    }
    pal_free(c);
}

void pal_ctx_idle(void)
{
    /* 没有就绪任务时不要空转烧核；1ms 粒度对本工程的调度精度足够 */
    Sleep(1);
}
