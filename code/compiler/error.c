/******************************************************************************
 * 文件名:   error.c
 * 描述:     编译器诊断信息输出管理实现 —— 机制层。
 *
 * 职责：产生诊断（格式化正文、计数）并交付给 sink；不决定诊断如何呈现。
 *       默认 sink 走 PAL 的 pal_printf，行为与历史上的"库内直接 printf"一致，
 *       故现有 driver（compiler_main.c）不注册也能照常工作。
 ******************************************************************************/
#include "error.h"
#include <stdarg.h>

static int         g_err_count = 0;
static comp_diag_fn g_sink     = 0;   /* 0 = 未注册，用默认 sink */
static void*        g_sink_ctx = 0;

/* ===================== 默认 sink（回落行为） ===================== */
/* 未注册时的呈现策略：中文标签 + 行号 + 正文，经 pal_printf 落地。
 * 注意：这是【策略】而非机制，仅作为兼容旧行为的默认；
 *       driver 应通过 comp_diag_set 提供自己的实现（测试收集、IDE 结构化等）。 */
static void default_sink(void* ctx, comp_severity_t severity, int line,
                         comp_err_code_t code, const char* msg)
{
    (void)ctx;
    const char* tag = (severity == COMP_DIAG_WARN) ? "编译器警告" : "编译器错误";
    if (line > 0) pal_printf("[%s] 行 %d: ", tag, line);
    else          pal_printf("[%s] ", tag);
    if (code != COMP_ERR_NONE) pal_printf("(%s) ", comp_err_name(code));
    pal_printf("%s\n", msg);
}

/* ===================== 注册接口 ===================== */
comp_diag_fn comp_diag_set(comp_diag_fn fn, void* ctx)
{
    comp_diag_fn old = g_sink;
    g_sink     = fn;      /* NULL 即恢复默认 */
    g_sink_ctx = ctx;
    return old;
}

/* ===================== 内部：格式化 + 投递 ===================== */
/* 把变参格式化到静态缓冲后交给 sink。历史上 comp_error 直接 pal_printf，
 * 这里改为"先格式化成 msg，再交付"，使 sink 能拿到结构化字段而非字节流。 */
static void emit(comp_severity_t severity, int line, comp_err_code_t code,
                 const char* fmt, va_list ap)
{
    static char msg[COMP_DIAG_MSG_MAX];
    pal_vsnprintf(msg, (uint32_t)sizeof(msg), fmt, ap);
    msg[COMP_DIAG_MSG_MAX - 1] = 0;      /* 防截断后无终止符 */

    comp_diag_fn sink = g_sink ? g_sink : default_sink;
    sink(g_sink_ctx, severity, line, code, msg);
}

/* ===================== 对外报告接口 ===================== */
comp_err_code_t comp_error(int line, comp_err_code_t code, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(COMP_DIAG_ERROR, line, code, fmt, ap);
    va_end(ap);
    g_err_count++;
    return code;
}

void comp_warn(int line, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(COMP_DIAG_WARN, line, COMP_ERR_NONE, fmt, ap);
    va_end(ap);
}

int  comp_error_count(void) { return g_err_count; }
void comp_error_reset(void) { g_err_count = 0; }

/* ===================== 可读名（供 sink 呈现） ===================== */
const char* comp_err_name(comp_err_code_t code)
{
    switch (code) {
        case COMP_ERR_NONE:       return "NONE";
        case COMP_ERR_SYNTAX:     return "SYNTAX";
        case COMP_ERR_UNDECLARED: return "UNDECLARED";
        case COMP_ERR_TYPE:       return "TYPE";
        case COMP_ERR_REDEF:      return "REDEF";
        case COMP_ERR_INTERNAL:   return "INTERNAL";
        default:                  return "UNKNOWN";
    }
}

const char* comp_severity_name(comp_severity_t sev)
{
    switch (sev) {
        case COMP_DIAG_WARN:  return "WARN";
        case COMP_DIAG_ERROR: return "ERROR";
        default:              return "?";
    }
}
