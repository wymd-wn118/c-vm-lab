/******************************************************************************
 * 文件名:   VM_MM.c
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-7-25
 * 描述:     内存管理实现：位图 + 1/2/4 固定池 + 可扩展句柄表 + 变长区 + GC。
 * 版本:     0.0.1
 * 版权:     无
 ******************************************************************************/
#include "VM_MM.h"
#include "DEBUG.h"
#include "PAL.h"   /* 字节操作走 PAL，避免引入 <stdlib.h>/<string.h> */

#define VM_MM_MAGIC      0x4D4D3142u   /* 魔数 "MM1B" */
#define VM_MM_MAX_HT_CAP 512u           /* 句柄表容量硬上限（512 槽，长到顶即 HT_FULL） */

/* 固定池元素个数：编译期定死 → 尺寸类固定，无外部碎片。 */
#define MM_POOL1_CNT 600
#define MM_POOL2_CNT 300
#define MM_POOL4_CNT 300
#define MM_HT_INIT_CAP 256u             /* 句柄表初始容量（按需翻倍，上限 VM_MM_MAX_HT_CAP） */

/* ===================== 内部工具 ===================== */
static vm_mm_hdr_t* mm_hdr(vm_t* vm) { return (vm_mm_hdr_t*)vm->data.base; }
static uint8_t*     mm_bitmap(vm_t* vm) { return vm->data.base + mm_hdr(vm)->bitmap_off; }

static void bit_set(vm_t* vm, uint32_t bit, int v) {
    uint8_t* b = mm_bitmap(vm);
    uint32_t by = bit >> 3, m = 1u << (bit & 7);
    if (v) b[by] |= m; else b[by] &= (uint8_t)~m;
}
static int bit_get(vm_t* vm, uint32_t bit) {
    return (mm_bitmap(vm)[bit >> 3] >> (bit & 7)) & 1;
}
/* 标记 [start, start+len) 的占用位 */
static void mark_range(vm_t* vm, uint32_t start, uint32_t len, int v) {
    for (uint32_t i = 0; i < len; i++) bit_set(vm, start + i, v);
}
/* 检查 [start, start+len) 是否全空闲 */
static int range_free(vm_t* vm, uint32_t start, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) if (bit_get(vm, start + i)) return 0;
    return 1;
}
/* 句柄项：每项 8 字节 = [ptr(4), size(4)]，句柄 1 起、下标 0 保留 */
static uint32_t* ht_entry(vm_t* vm, uint32_t h) {
    vm_mm_hdr_t* m = mm_hdr(vm);
    return (uint32_t*)(vm->data.base + m->ht_off + (h - 1) * 8);
}

/* ===================== 初始化 =====================
 * 布局：data 区内（单位：字节）
 *   [0, ht_off)                固定左侧：header + 1/2/4 池（偏移固定，仅 MM 内部读写）
 *   [ht_off, var_break)         句柄表占用：[ht_off, ht_off+ht_cap*8)，随扩容右移
 *   [var_break, bitmap_off)     变长区（malloc / 函数帧），向下生长，越界即碰撞
 *   [bitmap_off, total)         位图区：覆盖整片空间的占用位（位于右侧）
 * 扩容时只有右侧（位图位置 + 变长区）会动，固定左侧偏移不变，
 * 所以 VM_MM 重定位只需改 total / 位图偏移，不必搬动已有块。 */
void vm_mm_init_layout(vm_t* vm, const vm_static_layout_t* layout)
{
    /* 池大小：优先用镜像头的布局表（layout），无镜像时回退 config 固定值（测试桩数据） */
    uint32_t p1 = layout ? layout->pool1_cnt : MM_POOL1_CNT;
    uint32_t p2 = layout ? layout->pool2_cnt : MM_POOL2_CNT;
    uint32_t p4 = layout ? layout->pool4_cnt : MM_POOL4_CNT;

    uint32_t total = vm->data.size;
    pal_memset(vm->data.base, 0, total);        /* 整片清零：位图随之全零（全空闲） */

    vm_mm_hdr_t* h = (vm_mm_hdr_t*)vm->data.base;
    h->magic      = VM_MM_MAGIC;
    h->total      = total;
    uint32_t hsz  = (sizeof(vm_mm_hdr_t) + 3u) & ~3u;
    h->bitmap_off = hsz;                        /* 临时值，稍后改到真正的位图位置 */
    h->bitmap_size = (total + 7u) / 8u;

    /* 固定池区：header 之后紧接 1/2/4 池，大小来自布局表 */
    /* 防御性钳制：Loader 已校验布局表，但这里也是任何未来调用方的
     * 信任边界。池数量过大时 off 会越过 total，bitmap_off 掉到 var_break 以下、
     * bitmap_size 下溢，mark_range() 就会写到 data 窗口之外。 */
    {
        uint64_t need = (uint64_t)hsz + (uint64_t)p1
                      + (uint64_t)p2 * 2u + (uint64_t)p4 * 4u;
        /* 句柄表紧跟在池区之后，必须一起算进来：若漏掉它，var_break 会越过
         * total，bitmap_off 被钳到 var_break，bitmap_size = total - bitmap_off
         * 立即下溢成天文数字，mark_range 就会写到 data 窗口之外。 */
        need += (uint64_t)MM_HT_INIT_CAP * 8u;
        if (need + 64u > (uint64_t)total) { p1 = p2 = p4 = 0; }
    }

    uint32_t off = hsz;
    h->pool1_off = off; h->pool1_cnt = p1; off += p1;
    h->pool2_off = off; h->pool2_cnt = p2; off += p2 * 2;
    h->pool4_off = off; h->pool4_cnt = p4; off += p4 * 4;
    off = (off + 3u) & ~3u;

    h->ht_off  = off;
    h->ht_cap  = MM_HT_INIT_CAP;
    h->ht_used = 0;
    h->ht_free = 0;
    h->ht_max  = VM_MM_MAX_HT_CAP;
    /* 变长区底 = 句柄表当前容量末尾（动态临界值，紧贴左侧）；
     * 不预先占掉 512 槽：先按初始容量排布，句柄表按需翻倍扩容并右移该临界值；
     * 右侧（变长区）一旦要越过它即判定碰撞 → VAR_FULL → 向 MM 申请扩 data 区。 */
    h->var_break = (h->ht_off + (uint32_t)h->ht_cap * 8u + 3u) & ~3u;

    /* 位图放在「动态区右侧」：紧贴 total 末尾往前取 bitmap_size 字节（4 字节对齐） */
    uint32_t bitmap_off = total - h->bitmap_size;
    bitmap_off = bitmap_off & ~3u;
    if (bitmap_off < h->var_break) bitmap_off = h->var_break;  /* 防止堆过小导致位图越界 */
    h->bitmap_off = bitmap_off;
    h->bitmap_size = total - bitmap_off;        /* 实际位图字节数（必须覆盖 [0,total) 全部） */

    h->gc_enabled = 1;   /* 默认开启，loader 可覆盖 */
    h->gc_runs    = 0;
    h->last_err   = VM_MM_ERR_NONE;

    /* 先标记固定区已占用：header 与池区 [0, pool1_off)、句柄表 [ht_off, ht_off+ht_cap*8)
       以及位图区 [bitmap_off, total)；池内空槽保持空闲，
       由 alloc_pool 逐个占用，变长区 [var_break, bitmap_off) 初始全空闲。 */
    mark_range(vm, 0, h->pool1_off, 1);
    mark_range(vm, h->ht_off, (uint32_t)h->ht_cap * 8u, 1);
    mark_range(vm, h->bitmap_off, total - h->bitmap_off, 1);

    /* 再标记静态全局变量已占用的池槽（布局表给出使用数），使运行期 ALLOC 从
     * 静态区之后开始分配，全局变量与动态分配互不踩踏（doc §4.1）。 */
    if (layout) {
        for (uint32_t i = 0; i < layout->static_pool1_used; i++) mark_range(vm, h->pool1_off + i * 1u, 1u, 1);
        for (uint32_t i = 0; i < layout->static_pool2_used; i++) mark_range(vm, h->pool2_off + i * 2u, 2u, 1);
        for (uint32_t i = 0; i < layout->static_pool4_used; i++) mark_range(vm, h->pool4_off + i * 4u, 4u, 1);
    }
    /* profiling：初始已提交字节 = 静态全局占用的池字节 + 静态变长区 */
    h->used_bytes = layout ? (layout->static_pool1_used * 1u
                            + layout->static_pool2_used * 2u
                            + layout->static_pool4_used * 4u
                            + layout->static_varlen_len) : 0u;
    h->used_peak = h->used_bytes;

    /* 把各类地址写进保留寄存器，供 LDRX/STRX 等裸内存指令使用。
     * 这些偏移都落在「固定左侧」，或只随整体重定位一起平移，
     * 故此处赋值一次即可长期有效。 */
    vm->cpu.R[VM_DATA]  = 0;               /* data 区基址（相对偏移 0） */
    vm->cpu.R[VM_POOL1] = h->pool1_off;    /* 1 字节池基址 */
    vm->cpu.R[VM_POOL2] = h->pool2_off;    /* 2 字节池基址 */
    vm->cpu.R[VM_POOL4] = h->pool4_off;    /* 4 字节池基址 */
    vm->cpu.R[VM_ZR]    = 0;               /* 零寄存器恒为 0 */
}

void vm_mm_init(vm_t* vm) { vm_mm_init_layout(vm, NULL); }

/* 重置堆：必须按「当前 header 里记录的池大小」重建，不能退回默认池。
 * 默认池(600/300/300 = 2400B)只适用于全尺寸 data 窗口；按镜像头精确分配的
 * 小窗口（例如 3.3KB）装不下它，会让 var_break 越过 total，进而 bitmap_size
 * 下溢成天文数字并越界写位图。只有从未建过堆（magic 不匹配）时才用默认池。 */
void vm_mm_reset(vm_t* vm)
{
    vm_mm_hdr_t* h = mm_hdr(vm);
    if (h->magic != VM_MM_MAGIC) { vm_mm_init(vm); return; }

    vm_static_layout_t l;
    pal_memset(&l, 0, sizeof l);
    l.pool1_cnt = h->pool1_cnt;
    l.pool2_cnt = h->pool2_cnt;
    l.pool4_cnt = h->pool4_cnt;
    /* static_*_used 保持 0：vm_reset 的语义是「清空全部内存」，
     * 静态全局变量的占用标记随之一并清掉，重新运行需重新装载。 */
    vm_mm_init_layout(vm, &l);
}

void vm_mm_set_gc(vm_t* vm, uint8_t enable) { mm_hdr(vm)->gc_enabled = enable ? 1 : 0; }

/* ===================== 池分配（固定尺寸，快速） ===================== */
static uint32_t alloc_pool(vm_t* vm, uint32_t off, uint32_t cnt, uint32_t elsize, uint32_t align)
{
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t base = off + i * elsize;
        if (base + elsize > off + cnt * elsize) break;
        if (base % align != 0) continue;
        if (range_free(vm, base, elsize)) {
            mark_range(vm, base, elsize, 1);
            return base;
        }
    }
    return 0;
}

/* ===================== 变长区分配（自位图处 bitmap_off 向下找） ===================== */
static uint32_t alloc_var(vm_t* vm, uint32_t nbytes, uint32_t align)
{
    vm_mm_hdr_t* h = mm_hdr(vm);
    if (nbytes == 0) nbytes = 1;
    if (align == 0) align = 1;
    if (align > 4)  align = 4;
    /* 变长区可用空间 = [var_break, bitmap_off) */
    if (nbytes > (h->bitmap_off - h->var_break)) return 0;

    for (uint32_t c = h->bitmap_off - nbytes; c >= h->var_break; c--) {
        if (c % align != 0) continue;
        if (range_free(vm, c, nbytes)) {
            mark_range(vm, c, nbytes, 1);
            return c;   /* var_break 是固定下界（= 句柄表当前末尾），不在此更新 */
        }
    }
    return 0;
}

/* ===================== 句柄分配（可动态扩展） ===================== */
static uint32_t ht_alloc(vm_t* vm, uint32_t ptr, uint32_t size)
{
    vm_mm_hdr_t* h = mm_hdr(vm);
    uint32_t hand;

    if (h->ht_free != 0) {                 /* 优先复用空闲句柄 */
        hand = h->ht_free;
        uint32_t* e = ht_entry(vm, hand);
        h->ht_free = e[0];                 /* 槽首字段存的是下一个空闲句柄 */
        e[0] = ptr; e[1] = size;
        return hand;
    }
    if (h->ht_used < h->ht_cap) {          /* 追加新槽 */
        hand = ++h->ht_used;
        uint32_t* e = ht_entry(vm, hand);
        e[0] = ptr; e[1] = size;
        return hand;
    }
    /* 句柄表满时按需翻倍：新槽位于 [ht_off+ht_cap*8, ht_off+new_cap*8)，
       要求该区间当前仍空闲（尚未被变长区占用）；扩容成功后把 var_break
       同步右移。若该区间已被变长区占住，则无法再扩，返回 0
       （HT_FULL：句柄耗尽；MM 只负责右侧数据区扩容，救不了这种情况）。 */
    if (h->ht_cap < h->ht_max) {
        uint32_t old_cap = h->ht_cap;
        uint32_t new_cap = old_cap * 2u;
        if (new_cap > h->ht_max) new_cap = h->ht_max;
        uint32_t grow_off = h->ht_off + (uint32_t)old_cap * 8u;
        uint32_t grow_len = (uint32_t)(new_cap - old_cap) * 8u;
        if (range_free(vm, grow_off, grow_len)) {
            mark_range(vm, grow_off, grow_len, 1);          /* 标记新增槽已占用 */
            h->ht_cap   = new_cap;
            h->var_break = (grow_off + grow_len + 3u) & ~3u; /* 临界值同步右移 */
            DBG(DEBUG_COMP_VM, DEBUG_LEVEL_DEBUG, "handle-table expand %u -> %u\n", old_cap, new_cap);
            hand = ++h->ht_used;
            uint32_t* e = ht_entry(vm, hand);
            e[0] = ptr; e[1] = size;
            return hand;
        }
        return 0;   /* 新槽区被变长区占用，无法扩容 */
    }
    return 0;   /* 已达到硬上限 */
}

/* ===================== GC 压缩（变长区原地搬） =====================
 * 仅在使用 malloc 失败时启用：自动把变长块搬到高地址端，腾出连续空隙。
 * 正确性（按地址升序 p1<p2<...<pk，长度 s1..sk，目标自顶向下排布）：
 *   pk -> [total-sk, total)，p_{k-1} -> [total-sk-s_{k-1}, ...)，...
 *   对任一 j，目标位置 = total - (s_j+...+sk)。
 *   由「块不重叠」有 p_{j+1} >= p_j + s_j，且 (s_{j+1}+...+sk) <= total - p_{j+1}
 *   可得 目标位置 - p_j >= s_j；即目标位置落在此块尾部之后的更高处。
 * 因此按高地址优先处理，搬运方向总是「低地址 → 高地址」。
 * 压缩从变长区顶端（= 虚拟堆顶）开始：变长块从堆顶向下长，地址升序即
 * 从底到顶；按高地址优先搬运，目标必在已处理过的更高地址处，
 * 每次覆盖的源数据都已搬走，故不会破坏尚未搬动的低地址块。
 * 目标区末尾（bitmap 处）降到 var_break 以上即止。 */
static void gc_compact(vm_t* vm)
{
    vm_mm_hdr_t* h = mm_hdr(vm);
    uint32_t vsize = h->bitmap_off - h->var_break;   /* 变长区 = [var_break, bitmap_off) */
    if (vsize == 0) return;

    /* 新断点 = 位图处（= 虚拟堆顶），按地址降序（高地址先搬）从顶端向下排。
       高地址先搬时，目标区永远在已处理的更高处，搬完的源随即作废，
       不会破坏尚未搬动的低地址块（memmove 语义）。 */
    uint32_t new_break = h->bitmap_off;
    for (uint32_t i = 1; i <= h->ht_used; i++) {
        uint32_t* e = ht_entry(vm, i);
        uint32_t ptr = e[0], size = e[1];
        /* 已释放的槽 size==0，其 ptr 字段存的是空闲链表指针，
           不能拿它当数据块搬运，否则会破坏空闲链表。 */
        if (size == 0) continue;          /* 空槽，跳过 */
        /* 池块位于固定左侧 [pool1_off, ht_off)，按地址直接寻址，压缩绝不能搬动它们：
         * 搬动会让句柄指向新地址、而原池槽仍标记占用（永久泄漏），并且破坏
         * 本循环依赖的「句柄递增 ⇒ 数据递减」不变量。 */
        if (ptr < h->ht_off) continue;
        new_break -= size;
        if (new_break != ptr) {
            /* 原位搬移（memmove 语义）：dst>src 时倒序拷贝，保证重叠安全 */
            uint8_t* d = vm->data.base + new_break;
            const uint8_t* s = vm->data.base + ptr;
            if (d > s) { for (uint32_t k = size; k > 0; k--) d[k-1] = s[k-1]; }
            else       { for (uint32_t k = 0; k < size; k++) d[k] = s[k]; }
        }
        e[0] = new_break;                  /* 更新句柄指向（值改变，句柄号不变） */
    }

    /* 重建变长区位图：先清空，再按搬运后的新地址重标 */
    mark_range(vm, h->var_break, h->bitmap_off - h->var_break, 0);
    for (uint32_t i = 1; i <= h->ht_used; i++) {
        uint32_t* e = ht_entry(vm, i);
        if (e[1] != 0) mark_range(vm, e[0], e[1], 1);   /* 按新地址重建位图 */
    }
    h->gc_runs++;
}

/* ===================== 申请 / 释放 ===================== */
uint32_t vm_mm_alloc(vm_t* vm, uint8_t type, uint32_t nbytes)
{
    vm_mm_hdr_t* h = mm_hdr(vm);
    uint32_t ptr = 0, size = 0;

    if (type == VM_MM_TYPE_1B)      { ptr = alloc_pool(vm, h->pool1_off, h->pool1_cnt, 1, 1); size = 1; }
    else if (type == VM_MM_TYPE_2B) { ptr = alloc_pool(vm, h->pool2_off, h->pool2_cnt, 2, 2); size = 2; }
    else if (type == VM_MM_TYPE_4B) { ptr = alloc_pool(vm, h->pool4_off, h->pool4_cnt, 4, 4); size = 4; }
    else { /* 变长区 */
        ptr = alloc_var(vm, nbytes, 4);
        size = nbytes ? nbytes : 1;
        if (ptr == 0 && h->gc_enabled) {   /* 失败先 GC 一次再试 */
            gc_compact(vm);
            ptr = alloc_var(vm, nbytes, 4);
            size = nbytes ? nbytes : 1;
        }
    }
    h->last_err = VM_MM_ERR_NONE;
    if (ptr == 0) {
        /* 失败原因：池/句柄表满不可救；data 区不够给变长区（VAR_FULL），可扩容后再试 */
        h->last_err = (type == VM_MM_TYPE_VAR) ? VM_MM_ERR_VAR_FULL : VM_MM_ERR_POOL_FULL;
        return 0;
    }
    uint32_t hand = ht_alloc(vm, ptr, size);   /* 取得句柄；失败返回 0 */
    if (hand == 0) {
        /* 池槽/变长块在取句柄之前就已标记占用：取不到句柄必须回滚，
         * 否则每次失败都永久泄漏一个槽（曾复现：句柄表满时每次尝试漏一个 1B 槽）。 */
        h->last_err = VM_MM_ERR_HT_FULL;
        mark_range(vm, ptr, size, 0);
        return 0;
    }
    h->used_bytes += size;
    if (h->used_bytes > h->used_peak) h->used_peak = h->used_bytes;
    return hand;
}

void vm_mm_free(vm_t* vm, uint32_t hand)
{
    if (hand == 0 || hand > mm_hdr(vm)->ht_cap) return;
    uint32_t* e = ht_entry(vm, hand);
    uint32_t ptr = e[0], size = e[1];
    if (size == 0) return;                  /* 空槽，跳过 */
    mm_hdr(vm)->used_bytes -= size;
    mark_range(vm, ptr, size, 0);           /* 标记为空闲（数据不清零） */
    e[0] = mm_hdr(vm)->ht_free;             /* 压入空闲句柄链表 */
    e[1] = 0;
    mm_hdr(vm)->ht_free = hand;
}

/* ===================== STR/LDR 访问（句柄寻址读写） =====================
 * vm_mm_read / vm_mm_write 已改为 static inline 放在 VM_MM.h：VM.c 的派发热点
 * 与 WIN_syscall.c 可直接内联，构成「堆访问快速路径」；本文件不再提供跨 TU 版本。
 * 下列 alloc/free/stats 等函数改用自身的 mm_hdr/ht_entry 静态实现。 */

/* ===================== 统计 ===================== */
void vm_mm_stats(vm_t* vm, vm_mm_stats_t* s)
{
    vm_mm_hdr_t* h = mm_hdr(vm);
    pal_memset(s, 0, sizeof(*s));
    /* 池区：逐槽扫描占用位统计 */
    for (uint32_t i = 0; i < h->pool1_cnt; i++)
        if (bit_get(vm, h->pool1_off + i)) { s->pool1_used += 1; s->pool1_blocks++; }
    for (uint32_t i = 0; i < h->pool2_cnt; i++)
        if (bit_get(vm, h->pool2_off + i * 2)) { s->pool2_used += 2; s->pool2_blocks++; }
    for (uint32_t i = 0; i < h->pool4_cnt; i++)
        if (bit_get(vm, h->pool4_off + i * 4)) { s->pool4_used += 4; s->pool4_blocks++; }
    /* 变长区：扫描句柄表，size!=0 视为在用，size==0 视为已释放（空槽） */
    for (uint32_t i = 1; i <= h->ht_used; i++) {
        uint32_t* e = ht_entry(vm, i);
        /* 只有落在变长区的块才算 var；池句柄的 size 同样非 0（1/2/4），
         * 以前被误算进来过（100 B 变长 + 4 B 池被报成 104）。 */
        if (e[1] != 0 && e[0] >= h->var_break) { s->var_used += e[1]; s->var_blocks++; }
    }
    s->ht_used = 0; s->ht_free = 0;
    for (uint32_t i = 1; i <= h->ht_used; i++) {
        uint32_t* e = ht_entry(vm, i);
        if (e[1] != 0) s->ht_used++; else s->ht_free++;
    }
    s->ht_cap = h->ht_cap;
    s->ht_max = h->ht_max;
    s->gc_runs = h->gc_runs;
}

/* ===================== 扩容 / 重定位支持 ===================== */
uint8_t vm_mm_last_err(vm_t* vm) { return mm_hdr(vm)->last_err; }

/* 重建位图：清空整片位图，再标记固定区（header、池区、句柄表、位图本身）
 * 与所有在用块（size!=0）；其余变长区视为空闲（初始为空）。 */
static void mm_rebuild_bitmap(vm_t* vm)
{
    vm_mm_hdr_t* h = mm_hdr(vm);
    pal_memset(vm->data.base + h->bitmap_off, 0, h->bitmap_size);
    mark_range(vm, 0, h->pool1_off, 1);
    mark_range(vm, h->ht_off, (uint32_t)h->ht_cap * 8u, 1);
    mark_range(vm, h->bitmap_off, h->total - h->bitmap_off, 1);
    for (uint32_t i = 1; i <= h->ht_used; i++) {
        uint32_t* e = ht_entry(vm, i);
        if (e[1] != 0) mark_range(vm, e[0], e[1], 1);
    }
}

/* 原地扩展：base 不变，仅 total 变大；变长区随之向右扩展。
 * 物理 RAM 上的加长由 MM 完成，本函数只重建内部头、重绘位图，并按需 GC 腾出变长区。 */
void vm_mm_grow_inplace(vm_t* vm, uint32_t new_total)
{
    vm_mm_hdr_t* h = mm_hdr(vm);
    uint32_t bitmap_size = (new_total + 7u) / 8u;
    uint32_t bitmap_off  = (new_total - bitmap_size) & ~3u;
    if (bitmap_off < h->var_break) bitmap_off = h->var_break;
    /* vm->data.size 是 LDRX/STRX 边界检查与 OOM 路径使用的值。
     * 若仍留旧值，之后所有检查都按扩容前的窗口算，新拿到的内存等于不存在。 */
    vm->data.size  = new_total;
    h->total       = new_total;
    h->bitmap_off  = bitmap_off;
    h->bitmap_size = new_total - bitmap_off;
    mm_rebuild_bitmap(vm);
    if (h->gc_enabled) gc_compact(vm);   /* 扩容后做一次 GC，腾出连续变长区（可选，由 gc_enabled 决定） */
}

/* 整体重定位：内容已由 MM 拷到 new_base（容量 new_total），本函数只重建内部头
 * （total / 位图偏移）、重绘位图并按需 GC。固定左侧偏移不变，变长块的 [ptr,size]
 * 指针与长度原样保留，因此无需旧总大小参数。 */
void vm_mm_relocate(vm_t* vm, uint8_t* new_base, uint32_t new_total)
{
    vm->data.base = new_base;
    vm->data.size = new_total;
    vm_mm_hdr_t* h = mm_hdr(vm);
    uint32_t bitmap_size = (new_total + 7u) / 8u;
    uint32_t bitmap_off  = (new_total - bitmap_size) & ~3u;
    if (bitmap_off < h->var_break) bitmap_off = h->var_break;
    h->total       = new_total;
    h->bitmap_off  = bitmap_off;
    h->bitmap_size = new_total - bitmap_off;
    mm_rebuild_bitmap(vm);
    if (h->gc_enabled) gc_compact(vm);
}
