/******************************************************************************
 * 文件名:   MM.h
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-5-19
 * 描述:     物理内存管理器（MM）的对外接口，管理 VM 之外的一整片 RAM。
 *          用「块节点链表」记录每段的 start(起始偏移) / size(字节数) /
 *          task_id(0=空闲)，按任务维度分配、释放、回收，供上层组件申请连续内存。
 * 版本:     0.0.2
 * 版权:     无
 ******************************************************************************/
#ifndef MM_H
#define MM_H

#include "config.h"
#include "stdint.h"
#include "DEBUG.h"
/* 本模块只依赖 PAL，不引入 <stdlib.h>/<string.h>：字节拷贝走 pal_memcpy，
 * 不使用 malloc/free——物理内存由 MM.c 里的静态大数组持有（纯 C99 方案）。 */

/* 物理 RAM 总字节数：config.h 中 CONFIG_RAM_SIZE = 102400/8（个 8 字节单元），
 * 乘 8 得 100KB，即 OS 与所有任务共享的 MM 池上限。 */
#define MM_RAM_BYTES   (CONFIG_RAM_SIZE * 8)

/* 块节点：描述一段内存区（起始偏移、字节数、归属任务） */
typedef struct mm_node {
    uint32_t        start;   /* 相对 g_phys_ram 的起始偏移，按 4 字节对齐 */
    uint32_t        size;    /* 本块字节数，4 字节对齐 */
    uint32_t        task_id; /* 0 = 空闲块；非 0 = 该任务名下的已分配块 */
    uint8_t         region;      /* MM_REGION_INT / MM_REGION_EXT（阶段 4 启用） */
    uint8_t         flags;       /* MM_F_*：pinned 表示禁止搬迁 */
    struct mm_node* next;
} mm_node_t;

#define MM_TASK_FREE    0
#define MM_TASK_SYSTEM  1
#define MM_TASK_COMPILER 2      /* 宿主侧编译器：只做记账，不参与调度 */

/* 内存区域（阶段 0 只声明；阶段 4 才接真实外部 SRAM） */
#define MM_REGION_INT   0       /* 内部 RAM：快，放热数据 */
#define MM_REGION_EXT   1       /* 外部 SRAM（经 FSMC）：慢，容量大 */

/* 块标志 */
#define MM_F_NONE       0x00
#define MM_F_PINNED     0x01    /* 禁止搬迁：宿主 C 裸指针可能指向这里 */

/* 未显式登记策略的任务所使用的默认策略 */
#define MM_DEF_PRIO     3       /* 阶段 4：>=3 走内部 RAM，<3 走外部 SRAM */
#define MM_FAIL         0xFFFFFFFFu

/* 返回物理 RAM 池首地址（所有块偏移都以它为基准换算真实指针） */
uint8_t* mm_ram(void);

/* 初始化 MM：把整片 RAM 复位成「一个空闲块」的初始状态（重复调用无副作用） */
void mm_init(void);

/* 申请 size 字节并归属 task_id，返回起始偏移；失败返回 MM_FAIL。
 * 内部先按 4 字节向上取整，再沿块链表 first-fit 查找可用空闲块。 */
uint32_t mm_alloc(uint32_t task_id, uint32_t size);

/* 释放 start 起始的块：标记空闲并立即与相邻空闲块合并 */
void mm_free(uint32_t start);

/* 回收某任务名下的全部内存块（任务退出/崩溃时统一释放） */
void mm_reap(uint32_t task_id);

/* 为已分配块扩容：先尝试原地吞并右侧相邻空闲块；不够时另找新块并把数据搬过去，
 * 返回新的起始偏移。整片 RAM 都没有足够空间时返回 MM_FAIL（上层据此报 VM_OOM_ERROR）。
 * 注意：被标记 PINNED 的块禁止搬迁，无法原地扩容即失败。 */
uint32_t mm_grow_region(uint32_t task_id, uint32_t old_start, uint32_t old_size, uint32_t extra);

/* --- 阶段 0 新增：按任务策略 -----------------------------------------------
 * 一个任务的分配默认带什么 prio / region / flags。
 * MM 只认 task_id，绝不硬编码具体组件（VM / 编译器……）。
 * 新组件调一次本函数登记策略，之后照常使用 mm_alloc()。 */
void     mm_task_set_policy(uint32_t task_id, uint8_t prio, uint8_t region, uint8_t flags);

/* 显式指定 flags 的分配。mm_alloc() 签名不变，内部就是用本函数并传 MM_F_NONE，
 * 含义是「沿用该任务的默认策略」。 */
uint32_t mm_alloc_f(uint32_t task_id, uint32_t size, uint8_t flags);

/* 查询辅助（自省 / 测试用） */
uint8_t  mm_flags_of(uint32_t start);
uint8_t  mm_region_of(uint32_t start);
uint32_t mm_size_of(uint32_t start);

#endif
