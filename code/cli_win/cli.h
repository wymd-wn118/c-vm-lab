#ifndef CLI_H
#define CLI_H

#include <stdint.h>

/* ===================== 容量上限 ===================== */

/* 单行命令最大长度（含结尾 \0）。同时用于局部缓冲与 scanf 格式串宽度。 */
#define CLI_LINE_MAX      256

/* runtext 累积源码的最大长度 */
#define CLI_SRC_MAX       8192

/* ===================== 接口 ===================== */

/* @brief  CLI 主循环：初始化 MM / 调度器，读命令 -> 分发 -> 执行。
 * @return 0 正常退出（quit / exit / EOF）
 * @note   内部完成 mm_init() 与 PS_Init()，调用方无需重复初始化。
 * @note   run / runtext / runimg 不再限制推进轮数：一直跑到任务自然结束
 *         （HALT 或出错），保留死循环程序的语义 —— 这正是多任务的价值所在。
 *         死循环时按 Ctrl+C 中断进程（等非阻塞输入实现后，可改为只中断
 *         当前动作而保留 CLI 与其它任务）。 */
int cli_run(void);

#endif /* CLI_H */
