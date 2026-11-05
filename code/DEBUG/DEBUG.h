/******************************************************************************
 * 文件名:   DEBUG.h
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-5-10
 * 修改时间: 2026-8-7
 * 描述:     项目级分层调试输出框架（OS 运行时与宿主工具共用）。
 *           二维过滤：级别(level) × 组件(component)，各组件可在 config.h 独立开关。
 *           输出经 PAL 接口（pal_printf / pal_vprintf），不依赖 <stdio.h>，跨平台。
 * 版本:     0.0.2
 * 版权:     无
 ******************************************************************************/



#ifndef DEBUG_H_
#define DEBUG_H_

/* 所有头统一使用裸文件名，依赖编译期 -I/代码根目录（code/）加入包含路径。
 * 例如 MSVC: /I"D:\vs\os_text1\code"；GCC: -Icode。 */
#include "config.h"
#include "PAL.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 级别（Level） ============================ */
/* 由低到高；仅“级别 >= DEBUG_LEVEL_THRESHOLD”者会被输出（低于者编译期剪枝）。 */
#define DEBUG_LEVEL_TRACE 0   /* 最细：函数进出、寄存器值、单条指令 */
#define DEBUG_LEVEL_DEBUG 1   /* 开发期诊断：窗口分配、堆布局变化 */
#define DEBUG_LEVEL_INFO  2   /* 运行里程碑：任务创建、镜像加载完成 */
#define DEBUG_LEVEL_WARN  3   /* 可恢复异常：mm_alloc 重试、GC 触发 */
#define DEBUG_LEVEL_ERROR 4   /* 错误但继续：编译失败、解析错误 */
#define DEBUG_LEVEL_FATAL 5   /* 致命：立即退出 */

/* ============================ 组件（Component） ============================ */
/* 与 config.h 中 DEBUG_<COMP> 开关一一对应；新增组件时两处同步添加。 */
typedef enum {
    DEBUG_COMP_OS     = 0,  /* OS 运行时总类（调度等） */
    DEBUG_COMP_VM     = 1,  /* 虚拟机解释器（VM.c / VM_MM.c） */
    DEBUG_COMP_MM     = 2,  /* 内存管理器（MM.c） */
    DEBUG_COMP_LOADER = 3,  /* 加载器（Loader.c） */
    DEBUG_COMP_COMP   = 4,  /* 编译器总类（compiler_main.c） */
    DEBUG_COMP_LEX    = 5,  /* 词法分析（lexer.c） */
    DEBUG_COMP_PARSE  = 6,  /* 语法分析（parser.c） */
    DEBUG_COMP_CG     = 7,  /* 代码生成（codegen.c） */
    DEBUG_COMP_ASM    = 8,  /* 汇编 / 发射（assembler.c / emit.c） */
    DEBUG_COMP_PAL    = 9,  /* PAL 后端自身 */
    DEBUG_COMP_COUNT  = 10 
} debug_comp_t;

/* —— 组件开关映射：把枚举值映射到 config.h 的 DEBUG_<COMP> 宏 —— */
#define DEBUG_COMP_ENABLED(comp)                                       \
    ((comp) == DEBUG_COMP_OS     ? DEBUG_OS     :                     \
     (comp) == DEBUG_COMP_VM     ? DEBUG_VM     :                     \
     (comp) == DEBUG_COMP_MM     ? DEBUG_MM     :                     \
     (comp) == DEBUG_COMP_LOADER ? DEBUG_LOADER :                     \
     (comp) == DEBUG_COMP_COMP   ? DEBUG_COMP   :                     \
     (comp) == DEBUG_COMP_LEX    ? DEBUG_LEX    :                     \
     (comp) == DEBUG_COMP_PARSE  ? DEBUG_PARSE  :                     \
     (comp) == DEBUG_COMP_CG     ? DEBUG_CG     :                     \
     (comp) == DEBUG_COMP_ASM    ? DEBUG_ASM    :                     \
     (comp) == DEBUG_COMP_PAL    ? DEBUG_PAL    :                     \
     CONFIG_OFF)

/* ============================ 级别/组件 → 短名 ============================ */
/* 仅靠 PAL 接口输出，不直接碰 va_list/va_start（避免与宿主 <stdarg.h> 在某些
 * 工具链下 va_list 类型错配；统一经 pal_printf 可变参出口即可）。 */
static inline const char* dbg_level_str(int level)
{
    switch (level) {
        case DEBUG_LEVEL_TRACE:   return "TRC";
        case DEBUG_LEVEL_DEBUG:   return "DBG";
        case DEBUG_LEVEL_INFO:    return "INF";
        case DEBUG_LEVEL_WARN:    return "WRN";
        case DEBUG_LEVEL_ERROR:   return "ERR";
        case DEBUG_LEVEL_FATAL:   return "FTL";
        default:                  return "???";
    }
}
static inline const char* dbg_comp_str(debug_comp_t comp)
{
    switch (comp) {
        case DEBUG_COMP_OS:       return "OS";
        case DEBUG_COMP_VM:       return "VM";
        case DEBUG_COMP_MM:       return "MM";
        case DEBUG_COMP_LOADER:   return "LD";
        case DEBUG_COMP_COMP:     return "CMP";
        case DEBUG_COMP_LEX:      return "LEX";
        case DEBUG_COMP_PARSE:    return "PAR";
        case DEBUG_COMP_CG:       return "CG";
        case DEBUG_COMP_ASM:      return "ASM";
        case DEBUG_COMP_PAL:      return "PAL";
        default:                  return "???";
    }
}

/* ============================ 输出后端分发（唯一出口） ============================ */
/* DBG 宏不再直接调用 pal_printf，而是统一收敛到本函数：
 *   新增 FILE / BUFFER 输出模式时只需在此补 case，调用点与 DBG 宏均无需改动
 *   （config.h 已预留 DEBUG_MODE_FILE / DEBUG_MODE_BUFFER 枚举）。
 * 位置说明：必须位于 dbg_level_str / dbg_comp_str 之后，故放在本节而非文件中部。
 * va_list 说明：这里的 va_start 与 pal_vprintf 处于同一 TU、同一 <stdarg.h>，
 *   不存在跨工具链 va_list 错配；原先"不碰 va_list"的顾虑针对的是在宏里手写
 *   va_list，本处为 static inline 函数，语义安全。
 * 换行约定：fmt 需自带结尾 '\n'（现有调用点均如此），后端不再额外追加，
 *   避免每条日志后多出一个空行。 */
static inline void dbg_emit_backend(int level, debug_comp_t comp,
                                    const char* file, int line,
                                    const char* fmt, ...)
{
    va_list ap;
    switch (DEBUG_OUTPUT_MODE) {
    case DEBUG_MODE_CONSOLE:
    default:
        /* 控制台：前缀 [级别][组件] 文件:行号: + 正文，全部经 PAL 落地 */
        pal_printf("[%s][%s] %s:%d: ",
                   dbg_level_str(level), dbg_comp_str(comp), file, line);
        va_start(ap, fmt);
        pal_vprintf(fmt, ap);
        va_end(ap);
        break;

    /* —— 预留：接入时在此补 case，其余代码零改动 ——
    case DEBUG_MODE_FILE:
        // TODO: PAL 提供文件写接口后接入
        break;
    case DEBUG_MODE_BUFFER:
        // TODO: PAL 提供内存环缓冲后接入（裸机无控制台场景）
        break;
    */
    }
}

/* ============================ 对外宏 ============================ */
/* DBG(comp, level, fmt, ...) —— 两级编译期剪枝 + 一级运行期组件过滤，关闭时零开销。
 *
 * 闸门 1（编译期）：总开关 DEBUG == CONFIG_ON。关则全部 DBG 展开为空，
 *                   格式字符串不进二进制（零开销）。
 * 闸门 2（编译期）：级别阈值。DBG 经 ## 把级别记号拼成
 *                   DBG_DISPATCH_DEBUG_LEVEL_XXX，该宏按"阈值 <= 本级别"
 *                   用 #if 二选一；不满足即展开为空，低于阈值的格式字符串
 *                   同样不进二进制（与 config.h 的注释一致）。
 * 闸门 3（运行期）：组件开关 DEBUG_COMP_ENABLED(comp)。它是编译期常量表达式，
 *                   编译器可常量折叠；-O0 下字符串可能仍保留，若要彻底剔除
 *                   请把 config.h 中对应 DEBUG_<COMP> 置为 CONFIG_OFF。
 *
 * 输出统一经 dbg_emit_backend（唯一出口），后端内部才调用 PAL。 */
#if (DEBUG == CONFIG_ON)

    /* 实际发射：组件过滤后交给后端 */
    #define DBG_EMIT(comp, level, ...)                                          \
        do {                                                                    \
            if (DEBUG_COMP_ENABLED(comp) == CONFIG_ON) {                         \
                dbg_emit_backend(level, comp, __FILE__, __LINE__, __VA_ARGS__);  \
            }                                                                   \
        } while (0)

    /* 各级别分派宏：按阈值在预处理期二选一（真·编译期剪枝） */
    #if (DEBUG_LEVEL_THRESHOLD <= DEBUG_LEVEL_TRACE)
        #define DBG_DISPATCH_DEBUG_LEVEL_TRACE(comp, ...) DBG_EMIT(comp, DEBUG_LEVEL_TRACE, __VA_ARGS__)
    #else
        #define DBG_DISPATCH_DEBUG_LEVEL_TRACE(comp, ...) do {} while (0)
    #endif

    #if (DEBUG_LEVEL_THRESHOLD <= DEBUG_LEVEL_DEBUG)
        #define DBG_DISPATCH_DEBUG_LEVEL_DEBUG(comp, ...) DBG_EMIT(comp, DEBUG_LEVEL_DEBUG, __VA_ARGS__)
    #else
        #define DBG_DISPATCH_DEBUG_LEVEL_DEBUG(comp, ...) do {} while (0)
    #endif

    #if (DEBUG_LEVEL_THRESHOLD <= DEBUG_LEVEL_INFO)
        #define DBG_DISPATCH_DEBUG_LEVEL_INFO(comp, ...) DBG_EMIT(comp, DEBUG_LEVEL_INFO, __VA_ARGS__)
    #else
        #define DBG_DISPATCH_DEBUG_LEVEL_INFO(comp, ...) do {} while (0)
    #endif

    #if (DEBUG_LEVEL_THRESHOLD <= DEBUG_LEVEL_WARN)
        #define DBG_DISPATCH_DEBUG_LEVEL_WARN(comp, ...) DBG_EMIT(comp, DEBUG_LEVEL_WARN, __VA_ARGS__)
    #else
        #define DBG_DISPATCH_DEBUG_LEVEL_WARN(comp, ...) do {} while (0)
    #endif

    #if (DEBUG_LEVEL_THRESHOLD <= DEBUG_LEVEL_ERROR)
        #define DBG_DISPATCH_DEBUG_LEVEL_ERROR(comp, ...) DBG_EMIT(comp, DEBUG_LEVEL_ERROR, __VA_ARGS__)
    #else
        #define DBG_DISPATCH_DEBUG_LEVEL_ERROR(comp, ...) do {} while (0)
    #endif

    #if (DEBUG_LEVEL_THRESHOLD <= DEBUG_LEVEL_FATAL)
        #define DBG_DISPATCH_DEBUG_LEVEL_FATAL(comp, ...) DBG_EMIT(comp, DEBUG_LEVEL_FATAL, __VA_ARGS__)
    #else
        #define DBG_DISPATCH_DEBUG_LEVEL_FATAL(comp, ...) do {} while (0)
    #endif

    /* 统一入口：level 作为 ## 的操作数不被宏展开，故拼得 DBG_DISPATCH_DEBUG_LEVEL_XXX */
    #define DBG(comp, level, ...) DBG_DISPATCH_##level(comp, __VA_ARGS__)

#else  /* 总开关关：全部展开为空，格式字符串不进二进制 */
    #define DBG(comp, level, ...) do {} while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_H_ */
