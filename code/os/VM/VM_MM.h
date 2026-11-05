/******************************************************************************
 * 文件名:   VM_MM.h
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-7-25
 * 描述:     虚拟机内存管理小模块（独立于 CPU）。
 *          管理 vm_t.data 指向的本程序私有堆：
 *            - 1 bit/byte 位图（vm_init 时按堆大小设定，覆盖整片空间）
 *            - 1/2/4 字节固定分级池（左侧，向上排，尺寸类固定→零外部碎片）
 *            - 可动态扩展的句柄表（4 字节槽：存指针；池区存数据，句柄区存指针）
 *            - 变长区（malloc / 函数帧；从堆顶向下倒序长）
 *          所有分配返回"相对地址"（句柄下标）。GC 可经 loader 对每程序单独配置。
 * 版本:     0.0.1
 * 版权:     无
 ******************************************************************************/
#ifndef VM_MM_H_
#define VM_MM_H_

#include "VM.h"
#include "vm_image.h"   /* vm_static_layout_t：编译器/Loader 共享的静态区布局契约 */

/* 分配类型（编译期由 ALLOC 的 type 操作数决定；运行期无特判） */
#define VM_MM_TYPE_1B  1   /* 1 字节池 */
#define VM_MM_TYPE_2B  2   /* 2 字节池 */
#define VM_MM_TYPE_4B  4   /* 4 字节池 */
#define VM_MM_TYPE_VAR 0   /* 变长区（malloc / 帧），走句柄 */

/* MM 运行期头部，固定放在 data 区最前端（offset 0） */
typedef struct {
    uint32_t magic;
    uint32_t total;          /* data 区总字节数 */
    uint32_t bitmap_off;     /* 位图起始偏移 */
    uint32_t bitmap_size;    /* 位图字节数 (= total/8) */
    uint32_t pool1_off, pool1_cnt;   /* 1B 池：cnt 个元素，每元素 1 字节 */
    uint32_t pool2_off, pool2_cnt;   /* 2B 池：cnt 个元素，每元素 2 字节 */
    uint32_t pool4_off, pool4_cnt;   /* 4B 池：cnt 个元素，每元素 4 字节 */
    uint32_t ht_off;         /* 句柄表起始偏移 */
    uint32_t ht_cap;         /* 句柄表当前容量（槽数，可动态扩展） */
    uint32_t ht_used;        /* 已用到的最大槽下标 */
    uint32_t ht_free;        /* 空闲句柄链表头（0=空） */
    uint32_t ht_max;         /* 句柄表容量上限（预留扩容区大小，固定） */
    uint32_t var_break;      /* 变长区底 = 句柄表当前容量末尾（动态临界值，紧贴左侧；右侧分配越界即碰撞） */
    uint8_t  gc_enabled;     /* 1=开启 GC（默认），0=关闭（实时性优先） */
    uint32_t gc_runs;        /* GC 触发次数统计 */
    uint32_t used_bytes;     /* 当前已提交堆字节数（静态全局 + 动态分配；O(1) 维护） */
    uint32_t used_peak;      /* 运行期 used_bytes 峰值（供 profiling RAM 报表） */
    uint8_t  last_err;       /* 最近一次 vm_mm_alloc 失败原因（供上层判断是否可重试） */
} vm_mm_hdr_t;

/* vm_mm_alloc 失败原因（写入 hdr.last_err）：仅 VAR_FULL 可通过扩大 data 区缓解 */
#define VM_MM_ERR_NONE      0
#define VM_MM_ERR_VAR_FULL  1   /* 变长区空间不足（data 区整体不够） */
#define VM_MM_ERR_HT_FULL   2   /* 句柄表满（512 并发存活句柄，增长无用） */
#define VM_MM_ERR_POOL_FULL 3   /* 固定池满（增长无用） */

/* 使用情况统计 */
typedef struct {
    uint32_t pool1_used, pool1_blocks;
    uint32_t pool2_used, pool2_blocks;
    uint32_t pool4_used, pool4_blocks;
    uint32_t var_used,  var_blocks;
    uint32_t ht_used,   ht_free;
    uint32_t ht_cap,    ht_max;   /* 句柄表当前容量 / 上限（验证自动扩容） */
    uint32_t gc_runs;
} vm_mm_stats_t;

/* 初始化 / 重置 MM（依据 vm->data 当前大小布置位图/池/句柄表/变长区）。
 *   - vm_mm_init(vm)         ：回退到 config 固定池大小（测试桩 / 无镜像场景）。
 *   - vm_mm_init_layout(vm,l) ：接收编译器在 .osimg 头写出的静态区布局表，
 *                               用其 pool*_cnt 布置池大小，并把「静态占用的槽」
 *                               标记已占用，使运行期 ALLOC 从静态区之后开始，
 *                               全局变量与动态分配互不踩踏（doc §4.1）。 */
void    vm_mm_init(vm_t* vm);
void    vm_mm_init_layout(vm_t* vm, const vm_static_layout_t* layout);
void    vm_mm_reset(vm_t* vm);

/* 设置 GC 开关（loader 对每个程序单独配置：实时性高→关，稳定性高→开） */
void    vm_mm_set_gc(vm_t* vm, uint8_t enable);

/* 分配：type 见 VM_MM_TYPE_*；nbytes 仅变长区使用。返回句柄(1..cap)，失败返回 0 */
uint32_t vm_mm_alloc(vm_t* vm, uint8_t type, uint32_t nbytes);

/* 释放句柄（数据块与句柄槽均回收，释放即可复用） */
void    vm_mm_free(vm_t* vm, uint32_t handle);

/* STR/LDR 经此访问：句柄→指针→读写（含块内越界检查）。
 * 改为 static inline 定义于头文件：VM.c 的派发热点(LDR/STR/LDRH/STRH)与
 * WIN_syscall.c 均可将其直接内联，构成「堆访问快速路径」（消除跨 TU 函数调用开销）。
 * 注意：VM_MM.c 的其它函数改用自身的 mm_hdr/ht_entry，不依赖此头内联版本。 */
static inline uint8_t vm_mm_read(vm_t* vm, uint32_t handle, uint32_t off, uint32_t* out)
{
    vm_mm_hdr_t* h = (vm_mm_hdr_t*)vm->data.base;
    if (handle == 0 || handle > h->ht_cap) return VM_DATA_OVERFLOW_ERROR;
    uint32_t* e = (uint32_t*)(vm->data.base + h->ht_off + (handle - 1) * 8);
    uint32_t ptr = e[0], size = e[1];
    if (ptr == 0) return VM_DATA_OVERFLOW_ERROR;
    /* off + 4 > size 在 off = 0xFFFFFFFC 时会回绕（off+4 == 0），
 * 使读写逃出数据块。故先判 off 上界，范围检查一律用减法（绝不用加法）。 */
    if (off > size || size - off < 4u) return VM_DATA_OVERFLOW_ERROR;
    uint32_t p = ptr + off;
    if (p < ptr || p > vm->data.size || vm->data.size - p < 4u)
        return VM_DATA_OVERFLOW_ERROR;
    const uint8_t* b = vm->data.base + p;
    /* 小端：与代码段取指(vm_rd_u32)、裸内存 LDRX/STRX 一致（x86 与 Cortex-M4 均为小端）。
     * 消除「同一 data 区用不同指令读出不同字节序」的历史不一致。 */
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}
static inline uint8_t vm_mm_write(vm_t* vm, uint32_t handle, uint32_t off, uint32_t val)
{
    vm_mm_hdr_t* h = (vm_mm_hdr_t*)vm->data.base;
    if (handle == 0 || handle > h->ht_cap) return VM_DATA_OVERFLOW_ERROR;
    uint32_t* e = (uint32_t*)(vm->data.base + h->ht_off + (handle - 1) * 8);
    uint32_t ptr = e[0], size = e[1];
    if (ptr == 0) return VM_DATA_OVERFLOW_ERROR;
    /* 同上：先判 off 上界再相减，避免 off+4 回绕导致越界读写 */
    if (off > size || size - off < 4u) return VM_DATA_OVERFLOW_ERROR;
    uint32_t p = ptr + off;
    if (p < ptr || p > vm->data.size || vm->data.size - p < 4u)
        return VM_DATA_OVERFLOW_ERROR;
    uint8_t* b = vm->data.base + p;
    /* 小端：与 LDRX/STRX 一致 */
    b[0] = val & 0xFF;         b[1] = (val >> 8)  & 0xFF;
    b[2] = (val >> 16) & 0xFF; b[3] = (val >> 24) & 0xFF;
    return 0;
}

/* 填充使用情况统计 */
void    vm_mm_stats(vm_t* vm, vm_mm_stats_t* s);

/* 读取最近一次分配失败原因（VM_MM_ERR_*） */
uint8_t vm_mm_last_err(vm_t* vm);

/* 原地扩展：data.base 不变，仅总容量增大（变长区向右侧腾出空间）。
 * 调用前由 MM 完成物理 RAM 上该段的原地加长；本函数只重建内部头/位图/GC。 */
void    vm_mm_grow_inplace(vm_t* vm, uint32_t new_total);

/* 整体重定位：内容已由 MM 拷贝到 new_base（容量 new_total），本函数只重建
 * 内部头（total/位图偏移）、重绘位图并可选 GC。固定左侧（池/句柄表）偏移不变，
 * 变长块句柄 [ptr,size] 原样保留，因此无需旧总大小参数。 */
void    vm_mm_relocate(vm_t* vm, uint8_t* new_base, uint32_t new_total);

#endif
