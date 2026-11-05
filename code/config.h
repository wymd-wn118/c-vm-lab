/******************************************************************************
 * 文件名:   config.h
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-5-10
 * 修改时间: 2026-5-10
 * 描述:     用于配置项目的一些内容
 * 版本:     0.0.1
 * 版权:     无
 ******************************************************************************/
#ifndef CONFIG_H_
#define CONFIG_H_
#include "configName.h"

//===================== DEBUG 总开关 =====================
// 总开关：CONFIG_ON 开启全部 DEBUG（仍受各组件开关与级别阈值约束）；
//         CONFIG_OFF 关闭全部 DEBUG（所有 DBG 宏编译期展开为空，零开销）。
// 注意：DEBUG 仅负责“可观测性”（打印给人看），绝不参与控制流；
//       错误判定一律由返回值/错误码体系决定，切勿用 DEBUG 输出与否做分支。
#define DEBUG CONFIG_ON

//===================== DEBUG 组件开关 =====================
// 每个子系统可独立开启/关闭，便于只调试某一层而不被其它噪音淹没。
// 仅当“总开关 DEBUG==CONFIG_ON 且 该组件开关==CONFIG_ON”时对应组件才输出。
#define DEBUG_OS     CONFIG_ON   // OS 运行时总类（调度等）
#define DEBUG_VM     CONFIG_ON   // 虚拟机解释器（VM.c / VM_MM.c）
#define DEBUG_MM     CONFIG_ON   // 内存管理器（MM.c）
#define DEBUG_LOADER CONFIG_ON   // 加载器（Loader.c）
#define DEBUG_COMP   CONFIG_ON   // 编译器总类（compiler_main.c）
#define DEBUG_LEX    CONFIG_ON   // 词法分析（lexer.c）
#define DEBUG_PARSE  CONFIG_ON   // 语法分析（parser.c）
#define DEBUG_CG     CONFIG_ON   // 代码生成（codegen.c）
#define DEBUG_ASM    CONFIG_ON   // 汇编 / 发射（assembler.c / emit.c）
#define DEBUG_PAL    CONFIG_ON   // PAL 后端自身

//===================== DEBUG 级别阈值 =====================
// 仅输出“级别 >= 阈值”的日志；低于阈值者在编译期被剪枝（格式串不进二进制）。
// 该剪枝由 DEBUG.h 的 DBG_DISPATCH_DEBUG_LEVEL_* 用 #if 实现，不是运行期 if。
// 级别由低到高：TRACE < DEBUG < INFO < WARN < ERROR < FATAL
// 例：阈值设为 DEBUG 时，TRACE 被剪枝，DEBUG 及以上均输出；
//     ERROR 及以上（含 FATAL）建议始终可见（阈值 <= ERROR）。
#define DEBUG_LEVEL_THRESHOLD DEBUG_LEVEL_DEBUG

//===================== DEBUG 输出模式 =====================
// 决定日志最终写到哪里。目前仅实现控制台（CONSOLE），
// 其余模式已预留枚举与分发接口，后续接入文件/内存环缓冲时只需补后端。
#define DEBUG_MODE_CONSOLE 1   // 控制台（经 PAL 的 pal_printf，跨平台）
#define DEBUG_MODE_FILE    2   // 文件（预留接口，暂未实现）
#define DEBUG_MODE_BUFFER  3   // 内存环缓冲（预留接口，暂未实现，适合裸机无控制台）
#define DEBUG_OUTPUT_MODE  DEBUG_MODE_CONSOLE

//使用平台
#define PLATFORM CONFIG_WINDOWS


//各类存储空间大小,单位字节
// 项目总运行内存 = 100KB（F407 级 MCU 目标；OS + 所有任务共享的 MM 池上限）
#define CONFIG_RAM_SIZE ((102400/8))

/* 内存管理器的块描述符数量上限（每个节点 = 一段已分配/空闲内存的管理头）。
 * 它不是数据区大小，而是【额外的静态开销】：
 *     g_nodes[MM_MAX_NODES]  sizeof(mm_node_t) = 24 B(64位宿主) / 20 B(32位MCU)
 *     1024 -> 24 KB / 20 KB      4096 -> 96 KB / 80 KB      8192 -> 192 KB / 160 KB
 * 注意：它和 g_phys_ram[CONFIG_RAM_SIZE*8] 都落在 .bss 里，两者相加才是总静态占用。
 * 在 192 KB RAM 的 MCU 上，4096 就已超出（124 KB + 96 KB > 192 KB），故默认保持 1024。
 * 节点耗尽的典型症状是 comp_malloc 因很小的请求（几十字节）失败。 */
#ifndef MM_MAX_NODES
#define MM_MAX_NODES 1024u
#endif
// 调用栈：正常栈结构，帧直接落在栈上（不再是仅存堆句柄），给 16KB 余量
#define VM_STACK_SIZE 16384
#define VM_DATA_SIZE 20000
#define VM_PC_SIZE 10000
#define VM_R_SIZE 33   // 增 1：为 GPR 顶端腾出零寄存器(VM_ZR)与裸地址基址寄存器(R19..R23)

#endif