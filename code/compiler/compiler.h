/* @file   compiler.h
 * @brief  编译器公共头：源码 -> 镜像字节的唯一公开接口。
 *
 * @note   编译器是「宿主侧构建工具」，仅依赖 vm_isa.h 的常量，
 *         不链接 VM.c / MM.c / Loader.c。
 * @note   禁库约束：除 PAL/<平台>/ 平台实现文件外，编译器任何文件都不得直接
 *         使用 <stdio.h>/<stdlib.h>/<string.h>/<ctype.h>，统一改走 PAL.h。
 */
#ifndef COMPILER_H_
#define COMPILER_H_

#include <stdint.h>   /* 定宽整数类型（C99 类型头，非禁用库函数） */
#include <stddef.h>   /* size_t 等类型（C99 类型头，非禁用库函数） */
#include "vm_isa.h"   /* 唯一共享依赖：指令集常量（操作码/寄存器/SYSCALL） */
#include "PAL.h"      /* 内存/字符串/ctype/数值解析/文件IO/格式化，全部走 PAL */

/* @brief 动态内存的唯一出口。
 * @note  comp_* 镜像 pal_* 的语义，但归属 OS 内存服务（MM）。只有 comp_mem.c
 *        允许 include MM.h，因此编译器保持自包含；将来换成宿主 malloc 实现，
 *        只需改这一个文件。 */
#include "comp_mem.h"

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif


/* @brief 聚合接口：源码 -> 镜像字节。
 *
 * @note  定位：把 lex/parse/codegen/assemble/emit 五个步骤 + 中间缓冲管理 +
 *        会话状态清理，收进一次调用。外部（CLI / 单测 / sandbox）只需
 *        include 本头。
 * @note  边界（不做什么）：
 *          - 不读文件、不写文件（文件 IO 归调用者）
 *          - 不执行 VM（那是 sandbox 层的事，拉进来会破坏编译器自包含）
 *          - 不暴露 AST / token 等内部类型
 *          - 不支持多实例 / 可重入（error.h 的 sink 是全局单例）
 */

/* @brief 单条错误正文上限（字节）。
 * @note  诊断细节（行号 + 原因）是给 LLM 自我修正用的，必须结构化，
 *        而不是让调用者去正则解析 stdout。 */
#define COMP_MSG_MAX 96

/* @brief 编译失败的类型：区分「源码有问题」和「编译器资源不足」。
 * @note  调用者需要这个区分——前者去改代码，后者去改环境。
 *        把后者报成前者，正是我们要消除的误导性诊断。 */
typedef enum {
    COMP_FAIL_NONE = 0,
    COMP_FAIL_SOURCE,     /* 用户源码有问题；err_line 有效 */
    COMP_FAIL_RESOURCE,   /* 编译器侧限制：OOM / 缓冲不足 */
    COMP_FAIL_INTERNAL    /* 不该发生：内部状态不一致 */
} comp_fail_kind_t;

/* @brief 一次编译的完整结果。 */
typedef struct {
    int  ok;                    /* 1 = 成功；0 = 失败 */
    int  n_errors;              /* 诊断总数（>0 即失败） */
    int  err_line;              /* 第一条错误的源码行号；0 = 无 */
    char err_msg[COMP_MSG_MAX]; /* 第一条错误的正文（已格式化，无行号前缀） */
    int  code_len;              /* 成功时：写入 out_img 的镜像字节数 */
    int  required;              /* 缓冲不足时：需要的字节数；成功时 = code_len */
    int  pool1, pool2, pool4;   /* 全局变量占用的 1/2/4 字节池槽数（供 Loader/CLI） */
    int  max_frame;             /* 编译期最大调用栈帧（供 Loader 算栈窗口） */
    comp_fail_kind_t fail_kind; /* 见 comp_fail_kind_t */
} comp_result_t;

/* @brief 编译一份源码到镜像字节。
 * @param src     NUL 结尾的源码文本（长度由内部 pal_strlen 求出，不信任外部传入）
 * @param out_img 输出缓冲，【必须是宿主内存】—— 不能来自 comp_malloc，
 *                因为本函数结束时 comp_reap_all() 会清空编译器的 MM 堆
 * @param out_cap 缓冲容量（字节）
 * @return 见 comp_result_t。缓冲不足时 ok=0 且 required=需要的字节数，
 *         调用者可按 required 重新分配后再调一次。
 * @note  生命周期：每次调用都是一次完整会话（内部重置错误计数、重建字符串池、
 *        结束时回收整个编译器堆），调用之间不残留状态。 */
comp_result_t compiler_build(const char* src, uint8_t* out_img, uint32_t out_cap);

#endif /* COMPILER_H_ */