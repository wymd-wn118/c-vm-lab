/******************************************************************************
 * 文件名:   Loader.h
 * 作者:     WoYongMengDie
 * 创建时间: 2026-7-24
 * 描述:     Loader——任务创建者（task creator）。
 *          向 MM 申请 code/data/stack 三块窗口并填充 vm_t，再调 vm_init 初始化 CPU 与堆。
 *          线程/任务的创建与回收由 OS 调度器驱动。
 *          分工：MM 只负责分配物理内存，VM 只负责取指执行，两层保持解耦。
 * 版本:     0.0.1
 ******************************************************************************/
#ifndef LOADER_H
#define LOADER_H


#include "VM.h"     /* vm_t、uint8_t */
#include "MM.h"     /* mm_alloc / mm_ram / task_id 归属标签 */
#include "VM_MM.h"  /* vm_mm_set_gc */

/* 任务加载配置：从 .loading 清单读取，描述一个程序所需的资源与运行参数。
 * 注意 max_frame_size 等布局信息由镜像自身携带，本结构不重复保存。 */
typedef struct {
    uint32_t task_id;          /* MM 任务拥有者标识 */
    uint8_t  gc_enable;        /* 1=启用 GC（垃圾回收），0=关闭 GC（手动管理） */
    uint32_t recursion_depth;  /* 最大递归深度 = max_frame_size * depth + margin；0=未指定 */
    uint8_t  profiling;        /* 1=开启性能剖析，记录步数与耗时 */
    uint32_t profile_steps;    /* profiling 步数预算，0=不限制剖析步数 */
    uint32_t stack_override;   /* 0=按递归深度自动计算栈大小；非0=覆写栈窗口尺寸 */
    uint32_t ram_size;         /* 目标 RAM 总量字节数（默认等于 CONFIG_RAM_SIZE） */
    const char* image;         /* .osimg 镜像路径，由 .loading 的 image 字段指定 */
    uint8_t  image_owned;      /* 1=image 字符串由本模块分配，需在使用后释放 */
} vm_task_cfg_t;

/*
 * @brief  创建任务的运行环境：向 MM 申请 code/data/stack 三窗口并填充 vm_t，再初始化 CPU 与堆。
 * @param  vm_t* vm        当前虚拟机实例
 * @param  uint32_t task_id 任务拥有者标识（由调用方提供，MM 仅作归属标签）
 * @param  uint8_t gc_enable 1=启用自动 GC（稳定性优先），0=关闭 GC（实时性优先）
 * @return uint8_t 0=VM_OK 成功；VM_LOAD_ERR_NOMEM=窗口申请失败
 * @note   本函数不读镜像、不填代码/数据内容，三窗口大小写死为 config 常量；
 *         与 loader_spawn 行为不完全一致（spawn 的栈已按 recursion_depth 动态算）。 */
uint8_t loader_create_task(vm_t* vm, uint32_t task_id, uint8_t gc_enable);

/*
 * @brief  直接给定 .osimg 路径加载并运行一个程序（构造最小 cfg 后收敛到 loader_spawn）。
 * @param  vm_t* vm           当前虚拟机实例
 * @param  const char* path   .osimg 镜像路径
 * @param  uint32_t task_id   任务拥有者标识
 * @param  uint8_t gc_enable  1=启用 GC，0=关闭 GC
 * @return uint8_t 错误码（0 表示成功，非 0 见 VM_xxx_ERROR）
 * @note   因 cfg 未含 recursion_depth/stack_override，栈走默认 VM_STACK_SIZE 分支。 */
uint8_t loader_load_image(vm_t* vm, const char* path, uint32_t task_id, uint8_t gc_enable);

/*
 * @brief  开启性能剖析，设置步数预算。
 * @param  uint32_t budget  剖析步数预算，0=不限制步数
 * @return 无（void）
 * @note   超过预算后由 vm_print_ram_report 输出统计信息。 */
void loader_enable_profiling(uint32_t budget);

/*
 * @brief  进程调度的最小执行单元：运行任务直到终结/出错。
 * @param  vm_t* vm        当前虚拟机实例
 * @param  uint32_t task_id 任务拥有者标识
 * @return uint8_t 错误码（0 表示正常停机，非 0 见 VM_xxx_ERROR）
 * @note   遇 VM_OOM_ERROR 按策略缩小分配后重试；MM 无法恢复 RAM 不足则升级为 VM_FATAL_ERROR。 */
uint8_t process_run(vm_t* vm, uint32_t task_id);

/*
 * @brief  生产主入口：解析 .loading 清单得到完整 cfg，再交给 loader_spawn 加载运行。
 * @param  vm_t* vm             当前虚拟机实例
 * @param  const char* loading_path  .loading 清单文件路径
 * @return uint8_t 错误码（0 表示成功，非 0 见 VM_xxx_ERROR）
 * @note   支持键：image/gc_enable/recursion_depth/profiling/profile_steps/stack_override/ram_size；
 *         若 image 字符串由解析器分配（image_owned），本函数负责 pal_free 回收。 */
uint8_t loader_load_loading_file(vm_t* vm, const char* loading_path);

/*
 *@brief 只输入uint8_t*格式指令方式的输入指令
 *@param vm_t* vm 当前虚拟机实例
 *@param uint32_t task_id 任务归属者标识
 *@param uint8_t gc_enable 1=开启 GC，0=关闭 GC
 *@param uint8_t* raw 指令内容
 *@param uint32_t sz 指令长度
 *@param uint32_t recursion_depth 递归深度
 *@return uint8_t 返回码（0 表示成功，非 0 为 VM_xxx_ERROR）
 */
uint8_t loader_load_mem(vm_t *vm, uint32_t task_id, uint8_t gc_enable, uint8_t *raw, uint32_t sz, uint32_t recursion_depth);

#endif
