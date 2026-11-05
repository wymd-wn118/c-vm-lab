/******************************************************************************
 * 文件名:   MM.c
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-5-19
 * 最后修改: 2026-8-2
 * 描述:     物理内存管理器：管理整片物理 RAM 的分配与回收
 *           - 物理内存用静态大数组 g_phys_ram 持有，块描述符用静态池 g_nodes，
 *             全程不调用 malloc/free（池耗尽即分配失败）
 *           - 分配：4 字节对齐 + first-fit，余量够大才切分；释放后合并相邻空闲块
 *           - 扩容：原地吞并右侧空闲块，否则另找新块并 pal_memcpy 搬迁（VM 的 OOM 退路）
 *           - 调试日志经 DEBUG_OUTPUT，STM32/DEBUG==0 时整段编译为空
 * 版本:     0.0.3
 * 版权:     无
 ******************************************************************************/
#include "MM.h"
#include "PAL.h"   /* pal_memcpy 等字节操作走 PAL，避免引入 <string.h> */

/* ===================== 物理 RAM ===================== */
/* 整片物理内存：OS 与所有任务共享，由 Loader 切出 prog/data/stack 三段窗口。
 * 放在 .bss 的静态数组里，链接期即确定大小，不做任何动态申请。 */
static uint8_t   g_phys_ram[MM_RAM_BYTES];

/* ===================== 节点池 ===================== */
/* 节点池：每个 mm_node_t 描述一段内存（start 偏移 + 归属任务），
 * 节点从静态池取用、经空闲链表复用，不调用 malloc/free；
 * 取不到节点时 mm_alloc 直接返回 MM_FAIL。 */
/* MM_MAX_NODES 定义在 config.h（越大越费静态 RAM，按需调整） */
static mm_node_t  g_nodes[MM_MAX_NODES];
static mm_node_t* g_node_free = NULL;   /* 空闲节点链表头：node_free 压入，node_alloc 弹出 */
static uint32_t   g_node_next = 0;       /* 节点池线性水位：从未使用过的节点下标 */
static mm_node_t* g_mm_head   = NULL;    /* 块链表头：按地址升序串起所有已分配/空闲块 */

/* ---------------------------------------------------------------------------
 * node_alloc: 取一个空闲节点
 *   优先从 g_node_free 链表弹出；链表空则从 g_nodes 池取下一个（线性水位）。
 *   取出的节点不清零，由调用方（mm_alloc / mm_grow_region）填好各字段。
 *   节点池上限 MM_MAX_NODES，耗尽时打印告警并返回 NULL。
 * ------------------------------------------------------------------------- */
static mm_node_t* node_alloc(void)
{
    mm_node_t* n;
    if (g_node_free) {
        n = g_node_free;
        g_node_free = n->next;
    } else {
        if (g_node_next >= MM_MAX_NODES) {
            DBG(DEBUG_COMP_MM, DEBUG_LEVEL_WARN, "node pool EXHAUSTED (%u/%u nodes used)\n",
                g_node_next, MM_MAX_NODES);
            return NULL;
        }
        n = &g_nodes[g_node_next++];
    }
    n->next = NULL;
    return n;
}

/* ---------------------------------------------------------------------------
 * node_free: 归还一个节点
 *   把节点压回 g_node_free 链表头，下一个 node_alloc 即可复用。
 * ------------------------------------------------------------------------- */
static void node_free(mm_node_t* n)
{
    n->next = g_node_free;
    g_node_free = n;
}

/* 返回物理 RAM 池首地址（块偏移都以它为基准换算真实指针） */
uint8_t* mm_ram(void) { return g_phys_ram; }

/* ===================== 按任务策略表（阶段 0） =====================
 * MM 只认 task_id，绝不硬编码具体组件。
 * 未登记的任务使用默认策略（MM_DEF_PRIO / INT / 无 flags）。
 * 新组件调一次 mm_task_set_policy() 登记即可。 */
#define MM_MAX_POLICY 16u
typedef struct {
    uint32_t tid;
    uint8_t  prio;
    uint8_t  region;
    uint8_t  flags;
    uint8_t  used;
} mm_policy_t;
static mm_policy_t g_policy[MM_MAX_POLICY];

static mm_policy_t* policy_find(uint32_t tid)
{
    for (uint32_t i = 0; i < MM_MAX_POLICY; i++)
        if (g_policy[i].used && g_policy[i].tid == tid) return &g_policy[i];
    return NULL;
}

void mm_task_set_policy(uint32_t task_id, uint8_t prio, uint8_t region, uint8_t flags)
{
    mm_policy_t* p = policy_find(task_id);
    if (!p) {
        for (uint32_t i = 0; i < MM_MAX_POLICY; i++) {
            if (!g_policy[i].used) { p = &g_policy[i]; p->used = 1; p->tid = task_id; break; }
        }
    }
    if (!p) return;                    /* 策略表满：沿用默认策略 */
    p->prio   = prio;
    p->region = region;
    p->flags  = flags;
}

/* 解析一次分配的有效策略：任务默认值与调用方显式传入的 flags 按位或。 */
static void policy_resolve(uint32_t task_id, uint8_t flags,
                           uint8_t* out_region, uint8_t* out_flags)
{
    mm_policy_t* p = policy_find(task_id);
    uint8_t prio   = p ? p->prio   : MM_DEF_PRIO;
    uint8_t region = p ? p->region : MM_REGION_INT;
    uint8_t f      = p ? p->flags  : MM_F_NONE;
    /* 阶段 4 才把 prio 映射成 region；在那之前 region 只能显式指定 */
    (void)prio;
    *out_region = region;
    *out_flags  = (uint8_t)(f | flags);
}

/* ===================== 块合并 ===================== */
/* ---------------------------------------------------------------------------
 * mm_coalesce: 合并相邻空闲块
 *   释放/扩容后遍历块链表，把地址上首尾相接且都空闲的块并成一个大块；
 *   否则反复 alloc/free 会把空间切碎，出现「总空闲够、但没有单块够」的失败。
 * ------------------------------------------------------------------------- */
static void mm_coalesce(void)
{
    mm_node_t* p = g_mm_head;
    while (p && p->next) {
        /* 只合并地址上真正相邻的块。缺少相邻性检查时，两个空闲块之间
         * 夹着一块在用块也会被并成一个跨过它的大节点；之后 mm_alloc()
         * 就会发出与在用块重叠的内存，一写就把活数据写坏
         * （堆碎片化后表现为崩溃 / 静默数据损坏）。 */
        if (p->task_id == MM_TASK_FREE && p->next->task_id == MM_TASK_FREE
            && p->start + p->size == p->next->start) {
            p->size += p->next->size;
            mm_node_t* t = p->next;
            p->next = t->next;
            node_free(t);
        } else {
            p = p->next;
        }
    }
}

/* ===================== 分配 API ===================== */
/* ---------------------------------------------------------------------------
 * mm_init: 初始化 MM
 *   仅在首次调用时生效：把整片物理 RAM 设为唯一一个空闲块
 *   (start=0, size=MM_RAM_BYTES, task_id=FREE)，重复调用直接返回。
 * ------------------------------------------------------------------------- */
void mm_init(void)
{
    if (g_mm_head) return;                 /* 已初始化则不再重复建链 */
    g_mm_head = node_alloc();
    g_mm_head->start   = 0;
    g_mm_head->size    = MM_RAM_BYTES;
    g_mm_head->task_id = MM_TASK_FREE;
    g_mm_head->region  = MM_REGION_INT;
    g_mm_head->flags   = MM_F_NONE;
    g_mm_head->next    = NULL;
    DBG(DEBUG_COMP_MM, DEBUG_LEVEL_INFO, "init: phys RAM = %u B (%u KB), one free region\n",
        MM_RAM_BYTES, MM_RAM_BYTES / 1024u);
}

/* ---------------------------------------------------------------------------
 * mm_alloc: 申请 size 字节，归属 task_id，返回起始偏移
 *   1) size 先按 4 字节向上取整（0 当 1 处理）
 *   2) 沿块链表 first-fit：取第一个 size >= 请求的空闲块
 *   3) 余量 >= 8 字节才切分，否则整块给出（避免产生无用的碎片节点）
 *   4) 成功返回 start 并把节点标记为已分配；一个都找不到则返回 MM_FAIL，
 *      同时打印诊断（请求大小与最大空闲块）
 *   返回的是相对偏移，真实地址 = mm_ram() + start，由调用方自行换算。
 * ------------------------------------------------------------------------- */
/* 兼容用的薄包装：现有 7 处调用点（Loader 6 处 + MM 内部 1 处）
 * 无需修改，自动拿到所属任务的默认策略。 */
uint32_t mm_alloc(uint32_t task_id, uint32_t size)
{
    return mm_alloc_f(task_id, size, MM_F_NONE);
}

uint32_t mm_alloc_f(uint32_t task_id, uint32_t size, uint8_t flags)
{
    uint8_t region, eff_flags;
    policy_resolve(task_id, flags, &region, &eff_flags);

    if (size == 0) size = 1;
    size = (size + 3u) & ~3u;              /* 4 字节对齐 */

    mm_node_t* p = g_mm_head;
    uint32_t max_free = 0;                 /* 记录最大空闲块，失败时用于打印诊断 */
    while (p) {
        if (p->task_id == MM_TASK_FREE) {
            if (p->size > max_free) max_free = p->size;
            if (p->size >= size) {
                uint32_t start = p->start;
                if (p->size - size >= 8) {     /* 余量够大才切分，否则整块给出 */
                    mm_node_t* node = node_alloc();
                    if (node == NULL) return MM_FAIL;  /* 节点池耗尽，分配失败 */
                    node->start   = p->start + size;
                    node->size    = p->size - size;
                    node->task_id = MM_TASK_FREE;
                    node->region  = MM_REGION_INT;   /* 空闲块不携带策略 */
                    node->flags   = MM_F_NONE;
                    node->next    = p->next;
                    p->next       = node;
                    p->size       = size;
                }
                p->task_id = task_id;
                p->region  = region;
                p->flags   = eff_flags;
                return start;
            }
        }
        p = p->next;
    }
    DBG(DEBUG_COMP_MM, DEBUG_LEVEL_ERROR, "alloc FAIL: task=%u need=%u B, largest free block=%u B\n",
        task_id, size, max_free);
    return MM_FAIL;
}

/* ---------------------------------------------------------------------------
 * mm_free: 释放 start 起始的块
 *   标记为空闲后立即做一次合并（coalesce），使相邻空闲块重新连成一片。
 * ------------------------------------------------------------------------- */
void mm_free(uint32_t start)
{
    mm_node_t* p = g_mm_head;
    while (p) {
        if (p->start == start) {
            if (p->task_id == MM_TASK_FREE) {
                /* 重复释放：此块已是空闲状态。只有当它此后没被重新分配时，
                 * 再标一次空闲才无害；若已被重新分配，我们就把一个在用块标成了空闲，
                 * 两次分配会重叠 → 静默数据损坏。
                 * 所以这里只大声报错，不再动节点。 */
                DBG(DEBUG_COMP_MM, DEBUG_LEVEL_ERROR,
                    "DOUBLE FREE: start=%u is already FREE (size=%u)\n", start, p->size);
                return;
            }
            p->task_id = MM_TASK_FREE;
            p->flags   = MM_F_NONE;      /* 释放后不得保留 PINNED */
            p->region  = MM_REGION_INT;
            mm_coalesce();
            DBG(DEBUG_COMP_MM, DEBUG_LEVEL_DEBUG, "free: start=%u (coalesced)\n", start);
            return;
        }
        p = p->next;
    }
    /* 没找到。两种可能，性质完全不同：
     *   (a) 良性 —— 该块早前被合并掉了，这个 start 已不对应任何节点；
     *   (b) BUG —— 调用方传了 MM 从未发出的地址，或已释放过的地址，
     *       而我们在静默泄漏。
     * 打印出来，(b) 才藏不住。 */
    DBG(DEBUG_COMP_MM, DEBUG_LEVEL_WARN,
        "free: start=%u NOT FOUND (coalesced away, or not an MM block?)\n", start);
}

/* ---------------------------------------------------------------------------
 * mm_reap: 回收某任务名下的全部块
 *   遍历链表把归属 task_id 的块改回空闲（任务退出或崩溃后调用），
 *   最后统一合并一次，避免残留块越积越多、可用空间被慢慢吃掉。
 * ------------------------------------------------------------------------- */
void mm_reap(uint32_t task_id)
{
    mm_node_t* p = g_mm_head;
    while (p) {
        if (p->task_id == task_id) {
            p->task_id = MM_TASK_FREE;
            p->flags   = MM_F_NONE;      /* 与 mm_free 相同：一并清掉策略 */
            p->region  = MM_REGION_INT;
        }
        p = p->next;
    }
    mm_coalesce();
}

/* ---------------------------------------------------------------------------
 * mm_grow_region: 为已分配块扩容（VM 的 OOM 退路）
 *   路线 1) 右侧紧邻空闲块够大：原地吞并，返回原 start，base 不变，
 *           调用方只需扩大窗口大小，无需搬数据
 *   路线 2) 原地不行但整片 RAM 还有空间：另申请新块，pal_memcpy 搬过去，
 *           返回新 start，调用方需按新地址重建内部结构
 *   整片 RAM 都找不到足够大的块则返回 MM_FAIL，由调用方报错终止。
 *   注意：MM 只做物理搬运，不感知 VM 内部结构；VM 收到新 start 后自行重建。
 * ------------------------------------------------------------------------- */
uint32_t mm_grow_region(uint32_t task_id, uint32_t old_start, uint32_t old_size, uint32_t extra)
{
    if (extra == 0) extra = 1;
    extra = (extra + 3u) & ~3u;               /* 4 字节对齐 */

    /* 1) 右侧紧邻空闲块够大则原地扩容，直接返回原 start */
    mm_node_t* p = g_mm_head;
    while (p) {
        if (p->start == old_start) {
            /* old_size 由调用方给出。若与节点记录不一致，下面的重定位路径
             * 会按错误长度拷贝 —— 宁可报错，也不要静默多拷/少拷。 */
            if (p->size != old_size) {
                DBG(DEBUG_COMP_MM, DEBUG_LEVEL_ERROR,
                    "grow FAIL: caller says old_size=%u but node start=%u has size=%u\n",
                    old_size, old_start, p->size);
                return MM_FAIL;
            }
            if (p->next && p->next->task_id == MM_TASK_FREE
                && p->next->start == p->start + p->size
                && p->next->size >= extra) {
                mm_node_t* nxt = p->next;
                if (nxt->size - extra >= 8) { /* 余量够大才切分 */
                    /* 扩容后的块自身也要改尺寸。缺了下面几行，节点仍留着旧 size，
                     * 而邻居的 start 已前移，多出来的字节不属于任何块：
                     * 它们从空闲链表中消失，mm_size_of() 也不再等于调用方到手的量。 */
                    nxt->start += extra;
                    nxt->size  -= extra;
                    p->size    += extra;
                } else {                      /* 余量太小：整块并入 */
                    p->size += nxt->size;
                    p->next = nxt->next;
                    node_free(nxt);
                }
                return old_start;             /* 原地扩容成功，返回原 start */
            }
            break;
        }
        p = p->next;
    }

    /* 2) 被标记 PINNED 的块绝不能搬迁：宿主 C 裸指针可能指向它
     * （编译器 AST / 符号表）。无法原地扩容就直接失败，
     * 而不是另分配 + memcpy（那等于把块搬走了）。 */
    if (p && (p->flags & MM_F_PINNED)) {
        DBG(DEBUG_COMP_MM, DEBUG_LEVEL_ERROR,
            "grow FAIL: task=%u start=%u is PINNED and cannot grow in place (+%u B)\n",
            task_id, old_start, extra);
        return MM_FAIL;
    }

    uint32_t new_size  = (old_size + extra + 3u) & ~3u;
    uint32_t new_start = mm_alloc(task_id, new_size);
    if (new_start == MM_FAIL) {
        DBG(DEBUG_COMP_MM, DEBUG_LEVEL_ERROR, "grow FAIL: task=%u start=%u old=%u B need +%u B (no %u B free anywhere)\n",
            task_id, old_start, old_size, extra, new_size);
        return MM_FAIL;
    }
    pal_memcpy(mm_ram() + new_start, mm_ram() + old_start, old_size);  /* 搬数据到新块 */
    mm_free(old_start);                                                  /* 释放旧块 */
    DBG(DEBUG_COMP_MM, DEBUG_LEVEL_DEBUG, "grow: task=%u relocated %u B -> start=%u (+%u B)\n",
        task_id, old_size, new_start, extra);
    return new_start;
}

/* --- 查询辅助（阶段 0）--------------------------------------------------- */
static mm_node_t* node_find(uint32_t start)
{
    mm_node_t* p = g_mm_head;
    while (p) { if (p->start == start) return p; p = p->next; }
    return NULL;
}

uint8_t mm_flags_of(uint32_t start)
{
    mm_node_t* p = node_find(start);
    return p ? p->flags : 0;
}

uint8_t mm_region_of(uint32_t start)
{
    mm_node_t* p = node_find(start);
    return p ? p->region : MM_REGION_INT;
}

uint32_t mm_size_of(uint32_t start)
{
    mm_node_t* p = node_find(start);
    return p ? p->size : 0;
}
