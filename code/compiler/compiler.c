/******************************************************************************
 * 编译器聚合接口：源码 -> 镜像字节。
 *
 * 把 lex -> parse -> codegen -> assemble -> emit 五个步骤、四种中间缓冲、
 * 五种会话状态（错误计数 / 诊断 sink / 字符串池 / MM 堆 / OOM 闩）全部收进
 * 一次调用。调用者只需要提供源码和一块宿主缓冲。
 *
 * 设计要点：
 *   1) 所有可分配的指针在开头置 NULL，失败统一 goto fail：清理只写一次，
 *      新增失败分支自动被兜住，不会漏释放。
 *   2) 诊断经 error.h 的 sink 收集成结构化字段（行号 + 正文），
 *      而不是让调用者去正则解析 stdout。
 *   3) 结尾恢复原 sink，否则下一次调用会往已经销毁的 comp_result_t 里写。
 *   4) comp_reap_all() 在最后无条件执行，所以 out_img 必须是宿主内存。
 ******************************************************************************/
#include "compiler.h"
#include "PAL.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "assembler.h"
#include "emit.h"
#include "comp_mem.h"
#include "error.h"
#include "strpool.h"
#include "DEBUG.h"       /* MEM_MARK：阶段内存快照（仅调试构建） */

/* 阶段内存快照：只在 DEBUG 构建里存在，与 DBG 共用 DEBUG_LEVEL_THRESHOLD，
 * release 下展开为空语句。存在理由：各阶段串行且自释放，"把各阶段需求
 * 相加"会严重高估；只有实测峰值能指出天花板在哪。 */
#if (DEBUG_LEVEL_THRESHOLD <= DEBUG_LEVEL_DEBUG)
    #define MEM_MARK(s)  comp_mem_dbg_mark(s)
#else
    #define MEM_MARK(s)  ((void)0)
#endif

/* @brief 设置编译结果：失败 + 诊断。
 * @param r 错误正文。
 * @param kind 失败类型。
 * @param msg 诊断正文。 */
static void set_err(comp_result_t* r, comp_fail_kind_t kind, const char* msg)
{
    r->fail_kind = kind;
        /* 绝不覆盖 sink 已经产出的诊断：那条带源码行号，比 "parse failed" 这类
     * 笼统的内部消息有用得多。只在 err_msg 仍为空时才填。 */
    if (r->err_msg[0] != 0) return;
    pal_strncpy(r->err_msg, msg, COMP_MSG_MAX - 1);
    r->err_msg[COMP_MSG_MAX - 1] = 0;
}

/* 诊断收集器：只保留第一条错误的行号与正文，同时统计总数。
 * 警告不计入失败判定。 */
static void first_err_sink(void* ctx, comp_severity_t sev, int line,
                           comp_err_code_t code, const char* msg)
{
    comp_result_t* r = (comp_result_t*)ctx;
    (void)code;
    if (sev != COMP_DIAG_ERROR) return;
    r->n_errors++;
    if (r->err_line == 0) {
        r->err_line = line;
        pal_strncpy(r->err_msg, msg, COMP_MSG_MAX - 1);
        r->err_msg[COMP_MSG_MAX - 1] = 0;
    }
}

/* @brief 编译器入口：源码 -> 镜像字节。
 * @param src 源码字符串。
 * @param out_img 镜像字节缓冲。
 * @param out_cap 镜像字节缓冲容量。
 * @return static comp_result_t 编译结果。 
 * @note 调用者负责释放 out_img 内存。
 *       -
 * */
comp_result_t compiler_build(const char* src, uint8_t* out_img, uint32_t out_cap)
{
    comp_result_t r;
    pal_memset(&r, 0, sizeof r);

    /* 所有可能分配的指针先置 NULL，fail 路径据 NULL 判断是否已分配 */
    token_t*     toks      = NULL;
    prog_t*      ast       = NULL;
    char*        asm_buf   = NULL;
    uint8_t*     code      = NULL;
    comp_diag_fn prev_sink = NULL;
    int          session   = 0;

    /* --- 参数校验 --- */
    if (!src || !out_img || out_cap == 0) {
        set_err(&r, COMP_FAIL_INTERNAL, "invalid argument: src/out_img/out_cap");
        return r;
    }

    /* --- 会话开始 --- */
    comp_mem_init();                       /* 幂等：注册编译器任务策略 */
    sp_init();                             /* 字符串池：lex/parse/codegen 全程可用 */
    comp_error_reset();                    /* g_err_count 是全局的，必须清零 */
    prev_sink = comp_diag_set(first_err_sink, &r);
    session = 1;

    /* --- 1) 词法 --- */
    int ntok = 0;
    toks = lex(src, &ntok);
    if (!toks) { set_err(&r, COMP_FAIL_RESOURCE, "lex failed (out of memory)"); goto fail; }
    MEM_MARK("lex");

    /* --- 2) 语法 --- */
    ast = parse(toks, ntok);
    MEM_MARK("parse");
    comp_free(toks); toks = NULL;          /* 建完 AST 后 token 不再需要 */

    /* 顺序要紧：先取源码级诊断。否则真正的语法错误（"第 12 行：expected ';'"）
     * 会被 "parse failed" 这种毫无指向性的消息覆盖掉。 */
    /* OOM 必须排在语法错误之前。内存不足会让 parse_stmt() 返回 NULL，parser
     * 随即跳出函数体循环、expect('}') 失败 —— 报出来是一条「假的语法错误」。
     * 实测：20 个函数（约 180 个符号）的程序会报 "expected '}'"，而那个 '}'
     * 完全正常；用户照着改一百遍也没用。这里 OOM 是根因，语法错误是次生现象，
     * 所以要主动覆盖 err_msg 并清掉那个误导性的行号。 */
    if (comp_oom()) {
        r.n_errors  = comp_error_count();
        r.fail_kind = COMP_FAIL_RESOURCE;
        r.err_line  = 0;
        pal_strncpy(r.err_msg,
                    "out of memory during parse (heap space or MM node limit exhausted)",
                    COMP_MSG_MAX - 1);
        r.err_msg[COMP_MSG_MAX - 1] = 0;
        goto fail;
    }
    if (comp_error_count() > 0) {
        r.n_errors  = comp_error_count();
        r.fail_kind = COMP_FAIL_SOURCE;
        goto fail;
    }
    if (!ast)       { set_err(&r, COMP_FAIL_INTERNAL, "parser returned no AST without reporting an error"); goto fail; }
    if (ast->nfunc == 0) {
        set_err(&r, COMP_FAIL_SOURCE, "no function definitions found (a main() is required)");
        goto fail;
    }
    {
        /* codegen 的入口桩无条件发射 `CALL func_main`。缺 main() 时，失败要到
         * assemble() 阶段才以 "标签未定义" 的形式暴露 —— 读起来像编译器 bug，
         * 实际上是源码问题。趁 AST 还在手上，这里提前拦下。 */
        int has_main = 0;
        for (int i = 0; i < ast->nfunc; i++)
            if (ast->funcs[i]->is_main) { has_main = 1; break; }
        if (!has_main) {
            set_err(&r, COMP_FAIL_SOURCE,
                    "no main() function (the program entry point is required)");
            goto fail;
        }
    }

    /* --- 3) 代码生成 --- */
    /* 实测 180 个全局符号：源码 4577 B -> 汇编 6026 B（1.32 倍）。
     * 取 6 倍 + 8 KB 常数项（覆盖与源码长度无关的固定前导），约 5.9 倍余量。 */
    uint32_t src_len = pal_strlen(src);
    uint32_t asm_cap = src_len * 6u + 8192u;

    /* +1：assemble() 按 NUL 结尾扫描，必须给终止符留位置 */
    asm_buf = (char*)comp_malloc(asm_cap + 1u);
    if (!asm_buf) { set_err(&r, COMP_FAIL_RESOURCE, "out of memory (asm buffer)"); goto fail; }

    int asm_len = codegen(ast, asm_buf, (int)asm_cap,
                          &r.pool1, &r.pool2, &r.pool4);

    /* 缓冲估算不足属于「编译器自己的失误」，必须自愈而不是报错：
     * 翻倍重试即可，用户完全无感。codegen 每次调用都会重建符号表
     * （NGSYM/NLSM/NFMETA 归零），所以对同一份 AST 反复调用是安全的。
     * 截断判据用 >= cap-1：out() 把 k 钳到 room 后 G_OFF 最多等于 cap，
     * 而常见情形是 cap-1（最后一条指令差一个字节），只看 == cap 会漏掉。 */
    for (int retry = 0; retry < 3 && asm_len >= 0 &&
                        (uint32_t)asm_len >= asm_cap - 1u; retry++) {
        uint32_t want = asm_cap * 2u;
        /* 不设显式上限：asm_buf 走 comp_malloc，堆分配失败就是天然上限。
         * asm_cap 从 src_len*6+8K 起，翻 3 次也远不到 uint32 溢出。 */
        char* bigger = (char*)comp_malloc(want + 1u);
        if (!bigger) break;                        /* 真没内存，交给下面报错 */
        comp_free(asm_buf);                        /* 成功分配后才释放旧的 */
        asm_buf = bigger;
        asm_cap = want;
        asm_len = codegen(ast, asm_buf, (int)asm_cap,
                          &r.pool1, &r.pool2, &r.pool4);
        if (comp_error_count() > 0) break;         /* 重试中报错就停，别再翻倍 */
    }
    MEM_MARK("codegen");
    free_prog(ast); ast = NULL;            /* 释放后置 NULL，否则 fail 会重复释放 */

    if (comp_error_count() > 0) {          /* codegen 报错（类型/寄存器溢出等） */
        r.n_errors  = comp_error_count();
        r.fail_kind = COMP_FAIL_SOURCE;    /* codegen rejected the source */
        goto fail;
    }
    if (asm_len < 0 || (uint32_t)asm_len >= asm_cap - 1u) {
        /* 重试到上限仍然不够：这次确实无法自愈（堆耗尽，或源码规模超出容量） */
        set_err(&r, COMP_FAIL_RESOURCE,
                "codegen output truncated even after retries (source too large)");
        goto fail;
    }
    asm_buf[asm_len] = 0;                  /* assemble 需要 NUL 终止 */

    /* --- 4) 汇编 --- */
    /* 实测机器码约为汇编文本的 1/19；用 /8 + 8 KB 留 2 倍余量。
     * 若真不够，assemble() 返回 -2，是明确报错而非静默截断。 */
    uint32_t code_cap = (uint32_t)asm_len / 8u + 8192u;
    code = (uint8_t*)comp_malloc(code_cap);
    if (!code) { set_err(&r, COMP_FAIL_RESOURCE, "out of memory (code buffer)"); goto fail; }

    int clen = 0;
    int arc = assemble(asm_buf, code, (int)code_cap, &clen, &r.max_frame);

    /* 机器码缓冲估算不足同样要自愈：assemble() 没有任何静态状态，
     * 返回 -2 表示 "code buffer overflow"，翻倍重跑即可。 */
    for (int retry = 0; retry < 3 && arc == -2; retry++) {
        uint32_t want = code_cap * 2u;
        /* 无显式上限：comp_malloc 失败即天然上限（asm_cap 从 src_len*6+8K 起，翻 3 次远不到 uint32 溢出） */
        uint8_t* bigger = (uint8_t*)comp_malloc(want);
        if (!bigger) break;
        comp_free(code);
        code = bigger;
        code_cap = want;
        arc = assemble(asm_buf, code, (int)code_cap, &clen, &r.max_frame);
    }
    MEM_MARK("assemble");

    /* 汇编错误绝大多数是源码语义问题（标签未定义 / 重复定义 / 非法操作数），
     * 不是编译器缺陷。归类为 SOURCE，免得调用者去追一个并不存在的内部 bug。 */
    if (arc != 0) {
        /* -6 = split_lines 拆分汇编文本时内存不足（资源问题，改代码没用）；
         * 其余（标签未定义 / 重复标签 / 非法操作数）才是源码语义问题。 */
        set_err(&r, arc == -6 ? COMP_FAIL_RESOURCE : COMP_FAIL_SOURCE,
                asm_err_msg(arc));
        goto fail;
    }

    /* --- 5) 发射到调用者缓冲 --- */
    int total = emit_image_to_buf(out_img, out_cap, code, clen,
                                  r.pool1, r.pool2, r.pool4, r.max_frame);
    if (total == -3) { set_err(&r, COMP_FAIL_RESOURCE, "image length overflow"); goto fail; }
    if (total < 0) {                       /* 缓冲不足：告诉调用者要多大 */
        r.required = -total;
        set_err(&r, COMP_FAIL_RESOURCE, "output buffer too small");
        goto fail;
    }

    r.code_len = total;
    r.required = total;
    r.ok = 1;

    fail:
        /* 统一清理：无论成功失败都走到这里，只写一次 */
        if (code)    comp_free(code);
        if (asm_buf) comp_free(asm_buf);
        if (ast)     free_prog(ast);
        if (toks)    comp_free(toks);
        if (session) {
            comp_diag_set(prev_sink, NULL);    /* 恢复原 sink，防止写已失效的 r */
            sp_free();                         /* 先归还字符串池 */
            comp_reap_all();                   /* 再兜底回收整个编译器堆 */
        }
    return r;
}