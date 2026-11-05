#include "comp_mem.h"
#include "MM.h"          /* 唯一允许看到 OS 头的文件 */
#include "PAL.h"         /* pal_memset（comp_calloc 需要清零） */
#include "DEBUG.h"       /* DBG + DEBUG_LEVEL_THRESHOLD */

/* 编译器在 OS 内存服务里的身份与策略。
 * prio=2 / EXT / PINNED 必须一起给：少了 PINNED，comp_realloc 就可能
 * 搬动块，AST 里所有裸指针立刻悬空。 */
#define COMP_TID MM_TASK_COMPILER

static uint8_t g_inited = 0;

/* OOM 锁存。MM 一旦满足不了请求，编译器就该停下而不是硬撑：
 * 有的调用点检查返回值、有的不检查，拿着 NULL 继续往下必炸。
 * compiler_main() 会在阶段边界轮询 comp_oom() 并干净退出。 */
static int g_oom = 0;
int comp_oom(void) { return g_oom; }

/* 阶段内存快照：只在 DEBUG 构建里存在，与 DBG 共用 DEBUG_LEVEL_THRESHOLD，
 * 不额外引入配置项。记账要查 mm_size_of()，因为 comp_free 只拿到指针、
 * 拿不到原始请求大小（块自带 size 且 4 字节对齐）。
 * release 构建下 STAT_* 展开成空语句，热路径零开销、零符号。 */
#if (DEBUG_LEVEL_THRESHOLD <= DEBUG_LEVEL_DEBUG)
static uint32_t g_cur  = 0;
static uint32_t g_peak = 0;

#define STAT_ADD(n)   do { uint32_t _n = (n); g_cur += _n; \
                           if (g_cur > g_peak) g_peak = g_cur; } while (0)
#define STAT_SUB(n)   do { uint32_t _n = (n); \
                           g_cur = (_n <= g_cur) ? (g_cur - _n) : 0; } while (0)
#define STAT_RESET()  do { g_cur = 0; } while (0)

/* 阶段边界打一条 DEBUG 日志。phase_peak 是本阶段内的峰值（打完就把
 * 基线拉回当前值），这样能看出"哪个阶段把堆吃掉了"，而不是只看总量。 */
void comp_mem_dbg_mark(const char* stage)
{
    DBG(DEBUG_COMP_COMP, DEBUG_LEVEL_DEBUG,
        "mem: %-12s cur=%-7u phase_peak=%u\n",
        stage, (unsigned)g_cur, (unsigned)g_peak);
    g_peak = g_cur;
}
#else
#define STAT_ADD(n)   ((void)0)
#define STAT_SUB(n)   ((void)0)
#define STAT_RESET()  ((void)0)
#endif

void comp_mem_init(void)
{
    if (g_inited) return;
    mm_init();                       /* 先建空闲链表（幂等） */
    mm_task_set_policy(COMP_TID, 2, MM_REGION_EXT, MM_F_PINNED);
    g_inited = 1;
}

void* comp_malloc(uint32_t size)
{
    if (size == 0) size = 1;
    uint32_t off = mm_alloc_f(COMP_TID, size, MM_F_NONE);
    if (off == MM_FAIL) {
        /* 大声报错 + 锁存。现在每处构造点都检查指针，
         * parser / compiler_main 也会轮询 comp_oom()，让失败的分配
         * 直接终止编译，而不是级联成 NULL 解引用。 */
        pal_printf("comp_malloc FAILED: %u B (task %u) -- MM nodes/space exhausted?\n",
                   size, (unsigned)COMP_TID);
        g_oom = 1;
        return NULL;
    }
    STAT_ADD(mm_size_of(off));
    return (void*)(mm_ram() + off);
}

void* comp_calloc(uint32_t n, uint32_t size)
{
    /* 64 位乘积防溢出；保持 calloc 的清零契约 */
    uint64_t total = (uint64_t)n * (uint64_t)size;
    if (total == 0)          total = 1;
    if (total > 0xFFFFFF00u) return NULL;
    void* p = comp_malloc((uint32_t)total);
    if (p) pal_memset(p, 0, (uint32_t)total);      /* 调用方依赖清零 */
    return p;
}

/* 必须用它而不是 pal_strdup()：结果要用 comp_free() 释放，
 * 宿主 malloc 的字符串绝不能交给 MM
 * （这种错配曾产生 116 条 free: start=<garbage> NOT FOUND）。 */
char* comp_strdup(const char* s)
{
    if (!s) return NULL;
    uint32_t n = (uint32_t)pal_strlen(s);
    char* d = (char*)comp_malloc(n + 1u);
    if (!d) return NULL;
    pal_memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* 注：本模块曾实现 arena/bump allocator 版本，随后回退。
 * 在这块 100 KB first-fit 的 MM 上实测严格更差：arena 块无法单独释放
 * （存活的 AST 节点仍指向它们），峰值反而更高，而且每来一个新块都要
 * 一段新的连续空间。真实编译器敢用 arena 是因为它们的堆又大又连续；
 * 我们只有 100 KB 且是 first-fit，所以小块申请 + 真实释放更能耐受碎片。 */
void* comp_realloc(void* p, uint32_t old_bytes, uint32_t new_bytes)
{
    if (!p)             return comp_malloc(new_bytes);
    if (new_bytes == 0) { comp_free(p); return NULL; }

    uint32_t ob = (old_bytes + 3u) & ~3u;
    uint32_t nb = (new_bytes + 3u) & ~3u;
    if (nb <= ob) return p;                        /* 已经够大 */

    /* 我们的块是 PINNED，而 mm_grow_region() 拒绝搬迁 PINNED 块 ——
     * 正是这个拒绝保证了裸指针安全。所以走经典套路：
     * 新块 + 拷贝存活部分 + 释放旧块，返回「新指针」。
     * 调用方必须使用返回值；失败时旧块保持原样。 */
    void* np = comp_malloc(nb);
    if (!np) return NULL;
    pal_memcpy(np, p, (ob < nb) ? ob : nb);
    comp_free(p);
    return np;
}

void comp_free(void* p)
{
    if (!p) return;
    uint32_t off = (uint32_t)((uint8_t*)p - mm_ram());
    STAT_SUB(mm_size_of(off));   /* 容忍 double free：不会减成负数 */
    mm_free(off);
}

void comp_reap_all(void)
{
    mm_reap(COMP_TID);
    STAT_RESET();       /* 编译器持有的全部回收 */
}
