#ifndef COMP_MEM_H_
#define COMP_MEM_H_

#include <stdint.h>
#include "DEBUG.h"   /* DEBUG_LEVEL_THRESHOLD: 阶段内存快照的开关 */

/* ===================== 编译器内存门面 =====================
 * 编译器访问 OS 内存服务的唯一出口。
 *
 * 语义对齐 PAL 的 pal_*（comp_calloc 会把块清零），因此已有调用点
 * 只需把 pal_xxx 机械替换成 comp_xxx。
 *
 * 策略：MM_TASK_COMPILER = prio 2 + REGION_EXT + PINNED。PINNED 很关键，
 * 因为上层每个模块都握着指向这些块的裸指针。
 *
 * 为什么用门面而不是直接调 mm_*：
 *   - 让 code/compiler 自成一体（除本文件外不碰 OS 头）；
 *   - 映射关系（comp -> MM 任务号 + 策略）只存在一处，将来编译器
 *     跑在宿主（malloc）、跑在 OS 下（MM）、或跑在 VM 内（SYSCALL），
 *     都不必改动其它调用点。
 *
 * 关于 arena 的备注：本模块曾写过 bump allocator 版本，后来又回退了。
 * 在 100 KB first-fit 堆上实测比逐块分配更差 —— 见 comp_mem.c 的注释块。
 * 除非换成又大又连续的堆，不要「修」回 arena 版本。
 */

void  comp_mem_init(void);                                  /* 任务策略（幂等） */
void* comp_malloc(uint32_t size);                           /* 不清零 */
void* comp_calloc(uint32_t n, uint32_t size);               /* 清零，语义同 pal_calloc */

/* old_bytes 必须由调用方给出：光凭指针推不出来。重定位会返回「新指针」，
 * 调用方必须使用返回值；失败时原块保持不动。 */
void* comp_realloc(void* p, uint32_t old_bytes, uint32_t new_bytes);
char* comp_strdup(const char* s);   /* 必须用它而不是 pal_strdup：结果要用 comp_free 释放，
                                     * 宿主 malloc 出来的字符串绝不能交给 MM。 */

void  comp_free(void* p);                                   /* 真正释放 */
void  comp_reap_all(void);                                  /* 编译结束时一次性回收 */

/* 一份「发生过分配失败」的锁存标志。在各阶段边界轮询它，
 * 一旦为真就用诊断终止编译，而不是在 NULL 上继续往下跑。 */
int   comp_oom(void);

/* 阶段内存快照（仅调试构建）。
 *
 * 编译各阶段是串行且自释放的，所以「把各阶段需求相加」会严重高估；
 * 只有实测峰值才能指出天花板到底卡在哪。实测结论：瓶颈是 codegen 阶段的
 * AST（stmt_t 112 B / expr_t 80 B），而不是 assemble 里的 split_lines。
 *
 * 它不是功能接口，业务方无需了解：release 构建（阈值 > DEBUG）下这个符号
 * 不存在，记账代码也被编译期裁掉——与 DBG 共用同一个 DEBUG_LEVEL_THRESHOLD
 * 开关，不额外引入配置项。 */
#if (DEBUG_LEVEL_THRESHOLD <= DEBUG_LEVEL_DEBUG)
void  comp_mem_dbg_mark(const char* stage);
#endif

#endif /* COMP_MEM_H_ */
