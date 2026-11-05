/******************************************************************************
 * 文件名:   VM.h
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-5-10
 * 修改时间: 2026-7-19
 * 描述:     虚拟机的头文件
 * 版本:     0.0.2
 * 版权:     无
 ******************************************************************************/

#ifndef VM_H_
#define VM_H_

#include "config.h"
#include "stdint.h"
#include "DEBUG.h"
#include "MM.h"
/* 注意：VM.h 及其使用者禁用 <string.h>（跨平台/嵌入式仅允许 C99）。
 * 字节拷贝/填充请改走 PAL.h 的 pal_memset / pal_memcpy。 */

#include "vm_isa.h"   /* 指令操作码 / 特殊寄存器 / 错误码 / SYSCALL 编号 的单一真相源：
                       * 由 code/vm_isa.h 定义，VM 运行时与编译器共用，避免操作码漂移。 */

/* ===================== 虚拟机上下文结构 ===================== */
/* 一段连续内存区域：base 指向物理缓冲，size 为容量(bytes)，used 记录已占用 */
typedef struct {
    uint8_t*  base;
    uint32_t  size;
    uint32_t  used;
} vm_region_t;

/* CPU 上下文：寄存器文件（PC/SP/CMP_FLAG 均为其中特定下标） */
typedef struct {
    uint32_t R[VM_R_SIZE];
} vm_cpu_t;

/* 一台完整的虚拟机：CPU + 三段内存（程序/数据/栈） */
typedef struct vm_t {
    vm_cpu_t     cpu;
    vm_region_t  prog;   /* 程序内存 */
    vm_region_t  data;   /* 数据内存（堆） */
    vm_region_t  stack;  /* 栈内存 */
    uint32_t max_frame_size;   /* 镜像头：编译期全局最大调用栈帧字节数（含 ret+fp 头），供精确栈窗口计算 */

    /* profiling（可选）：测量真实运行时的栈/堆峰值与步数，供 F407 RAM 预算评估。
     * 仅在 profiling=1 时采集，生产运行零开销。 */
    uint32_t stack_peak;     /* 运行期观测到的最大 SP（字节） */
    uint32_t step_count;     /* 已执行指令数 */
    uint8_t  profiling;      /* 1=启用 profiling（记录峰值+步数预算） */
    uint32_t profile_budget; /* 步数预算（0=不限）；到顶返回 VM_PROFILE_DONE */
} vm_t;

/* 栈/数据窗口不再用全局变量描述；其边界随每任务内存视图（vm_t 内 prog/data/stack
 * 区域的 base/size）走，由 MM 分配、loader 填充，从而支持多线程互不干扰。 */

/* 全局虚拟机实例 */
extern vm_t g_vm;

/* SYSCALL 不再走上层注册回调，统一走 PAL 默认实现 pal_syscall
 * （见 PAL.h / PAL/windows/WIN_syscall.c）。 */

/* ===================== 公共 API ===================== */
void    vm_init(vm_t* vm);                                  /* CPU/堆初始化（内存窗口由 loader 申请填入） */
void    vm_reset(vm_t* vm);                                 /* 清空寄存器与全部内存 */
void    vm_load(vm_t* vm, const uint8_t* code, uint32_t len);/* 载入程序，PC 归零 */
uint8_t vm_step(vm_t* vm);                                  /* 执行单条指令，返回错误码(0=成功) */
uint8_t vm_run(vm_t* vm);                                   /* 连续执行直到 HALT/错误/步数上限 */
void    vm_print_ram_report(vm_t* vm);                     /* profiling 结束后打印 RAM 占用报表（对比项目总内存） */

/* 兼容旧接口的薄封装（单步执行 g_vm） */
uint8_t VM_RUN(void);

#endif
