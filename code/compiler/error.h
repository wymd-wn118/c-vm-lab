/******************************************************************************
 * 文件名:   error.h
 * 描述:     编译器诊断信息（Diagnostic）管理 —— 机制层。
 *
 * 定位：本模块是【面向用户的诊断】，不是【面向开发者的调试日志】。
 *       语法错误、类型不匹配、未声明标识符等必须告知写程序的人，属产品功能，
 *       不受 DEBUG 框架的总开关与级别阈值控制；否则 DEBUG=OFF 时编译器会变哑巴。
 *       （开发者的调试日志请走 DEBUG.h 的 DBG(DEBUG_COMP_LEX/PARSE/CG/ASM, ...)。）
 *
 * 机制 / 策略分离：
 *   编译器（库）= 机制：只有它知道"第几行缺了什么"，负责发现并产生诊断；
 *   调用方（driver）= 策略：决定诊断往哪去、什么格式、是否中断、何种语言。
 *   故诊断不在库内直接 pal_printf，而是经注册的 sink 交付；driver 不注册时
 *   回落到默认 sink（等同旧行为，向后兼容）。
 *
 *   这样做解决了两个具体问题：
 *     1) 可测试性：PAL 的 pal_capture 只捕获 SYSCALL 的 pal_putch，不捕获
 *        pal_printf，故写死 printf 时编译诊断无法自动化 diff；driver 注入
 *        收集式 sink 后即可像 main.c 比对 VM 输出那样比对诊断文本。
 *     2) 输出目标可换：将来编译器嵌入 OS、接入 IDE（结构化输出）或换语言文案，
 *        只需换 sink，库代码不动。
 *
 * 已知取舍：sink 为全局单例（与既有的 g_err_count 全局一致），不可重入、
 *   不支持多编译器实例并发。本项目单线程、教学用途，可接受。
 *   若将来需多实例/可重入，把 sink 收进 comp_ctx_t 并贯穿
 *   lex/parse/codegen/assemble 签名即可（调用点语义不变）。
 ******************************************************************************/
#ifndef COMPILER_ERROR_H_
#define COMPILER_ERROR_H_

#include "PAL.h"   /* pal_printf / pal_vsnprintf / pal_vprintf：默认 sink 的落地渠道 */

/* 编译器诊断码 */
typedef enum {
    COMP_ERR_NONE     = 0,
    COMP_ERR_SYNTAX,    /* 语法错误 */
    COMP_ERR_UNDECLARED,/* 变量/函数未声明 */
    COMP_ERR_TYPE,      /* 类型不匹配 */
    COMP_ERR_REDEF,     /* 重复定义 */
    COMP_ERR_INTERNAL   /* 内部错误（应为不可能到达） */
} comp_err_code_t;

/* 诊断严重级（供 sink 区分呈现，如 driver 可把 WARN 降级为静默） */
typedef enum {
    COMP_DIAG_WARN  = 0,   /* 警告：不中止编译 */
    COMP_DIAG_ERROR = 1    /* 错误：计入 comp_error_count，通常中止 */
} comp_severity_t;

/* 单条诊断的正文上限（超出截断，避免库内出现动态分配） */
#define COMP_DIAG_MSG_MAX 256

/* 诊断接收器（sink）。由 driver 实现并注册，决定诊断的呈现策略。
 *   ctx         注册时传入的上下文（如收集缓冲、输出流），库原样透传、不解释
 *   severity    COMP_DIAG_WARN / COMP_DIAG_ERROR
 *   line        源码行号；库不掌握行号时传 <= 0，sink 自行决定如何显示
 *   code        诊断码，无码者传 COMP_ERR_NONE
 *   msg         已格式化的正文（'\0' 结尾，不含行号前缀与换行）
 * 注：msg 指向库内部静态缓冲，sink 需自行复制，不可跨调用持有。 */
typedef void (*comp_diag_fn)(void* ctx, comp_severity_t severity, int line,
                             comp_err_code_t code, const char* msg);

/* 注册诊断接收器；fn 传 NULL 表示恢复默认 sink（pal_printf 输出中文诊断）。
 * 返回上一次注册的 sink，便于 driver 临时接管后再还原。 */
comp_diag_fn comp_diag_set(comp_diag_fn fn, void* ctx);

/* 报告一个编译错误（带源码行号），经 sink 交付。返回 code 便于调用方短路返回。
 * fmt 为 printf 风格格式串。 */
comp_err_code_t comp_error(int line, comp_err_code_t code, const char* fmt, ...);

/* 报告一个编译警告（不中止编译）。 */
void comp_warn(int line, const char* fmt, ...);

/* 累计错误数（供编译结束判断是否成功）。 */
int  comp_error_count(void);
void comp_error_reset(void);

/* 诊断码 → 简短可读名（供 sink 呈现；库不硬编码任何呈现文案）。 */
const char* comp_err_name(comp_err_code_t code);

/* 严重级 → 简短可读名（同上）。 */
const char* comp_severity_name(comp_severity_t sev);

#endif /* COMPILER_ERROR_H_ */
