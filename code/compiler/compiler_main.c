/******************************************************************************
 * 编译器命令行入口（CLI 薄包装）。
 *
 *   compiler [-o out.osimg] [-S] (file.lang | -c "src")
 *     -o : 写镜像文件（默认 a.osimg）
 *     -c : 内联源码
 *     -S : 只产出汇编文本到文件（-o 指定，缺省 out.asm），不生成镜像
 *
 * 本文件只做三件事：解析 argv、读源文件、调用 compiler_build。
 * 编译流水线本身在 compiler.c，本文件不再直接调用 lex/parse/codegen。
 * 唯一例外是 -S：它需要汇编文本，而 compiler_build 只产出镜像字节。
 ******************************************************************************/
#include "compiler.h"
#include "PAL.h"
#include "comp_mem.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "assembler.h"
#include "emit.h"
#include "strpool.h"

/* 镜像缓冲上限：输出缓冲必须是宿主内存（compiler_build 结尾会 reap MM 堆）。
 * 64 KB 对实测 180 符号（镜像 508 B）绰绰有余；不足时 compiler_build 会
 * 通过 required 告诉我们实际需要多大。 */
#define CLI_IMG_MAX (64u * 1024u)

static char* read_file(const char* path) {
    uint32_t sz;
    char* buf = pal_read_file(path, &sz);
    if (!buf) pal_printf("cannot open %s\n", path);
    return buf;
}

/* -S 专用：把汇编文本写到文件。
 * 不走 compiler_build，因为后者只产出镜像；这里复用同样的内部模块，
 * 但只需要走到 codegen 为止。 */
static int dump_asm(const char* src, const char* path) {
    comp_mem_init();
    sp_init();
    comp_error_reset();

    int ntok = 0;
    token_t* toks = lex(src, &ntok);
    if (!toks) {
        pal_printf("lex failed (out of memory)\n");
        sp_free(); comp_reap_all();
        return 1;
    }

    prog_t* ast = parse(toks, ntok);
    comp_free(toks);
    if (!ast || ast->nfunc == 0) {
        if (ast) free_prog(ast);
        pal_printf("parse failed\n");
        sp_free(); comp_reap_all();
        return 1;
    }

    uint32_t src_len = pal_strlen(src);
    uint32_t cap = src_len * 6u + 8192u;
    if (cap > (1u << 20)) cap = (1u << 20);
    char* buf = (char*)comp_malloc(cap);
    if (!buf) {
        free_prog(ast);
        pal_printf("out of memory (asm buffer)\n");
        sp_free(); comp_reap_all();
        return 1;
    }

    int p1 = 0, p2 = 0, p4 = 0;
    int len = codegen(ast, buf, (int)cap, &p1, &p2, &p4);
    free_prog(ast);

    int rc = 0;
    if (comp_error_count() > 0) {
        pal_printf("codegen failed (%d error(s))\n", comp_error_count());
        rc = 1;
    } else if (len < 0 || (uint32_t)len >= cap) {
        pal_printf("codegen output truncated (cap=%u B)\n", cap);
        rc = 1;
    } else if (pal_write_file(path, buf, (uint32_t)len) != 0) {
        pal_printf("cannot write %s\n", path);
        rc = 1;
    } else {
        pal_printf("assembly written to %s (%d B)\n", path, len);
    }

    comp_free(buf);
    sp_free();
    comp_reap_all();
    return rc;
}

int main(int argc, char** argv) {
    const char* infile  = NULL;
    const char* outfile = NULL;
    const char* csrc    = NULL;
    int S_flag = 0;
    for (int i = 1; i < argc; i++) {
        if (!pal_strcmp(argv[i], "-o") && i + 1 < argc) outfile = argv[++i];
        else if (!pal_strcmp(argv[i], "-S")) S_flag = 1;
        else if (!pal_strcmp(argv[i], "-c") && i + 1 < argc) csrc = argv[++i];
        else if (argv[i][0] != '-') infile = argv[i];
        else pal_printf("unknown arg %s\n", argv[i]);
    }

    const char* src;
    char* owned = NULL;
    uint8_t* heap_img = NULL;      /* 非 NULL 表示用了重试缓冲，退出前要释放 */
    if (csrc) src = csrc;
    else if (infile) { owned = read_file(infile); if (!owned) return 1; src = owned; }
    else {
        pal_printf("usage: compiler [-o out.osimg] [-S] (file.lang | -c \"src\")\n");
        return 1;
    }

    /* 先删旧产物：编译失败时不能留下上一次的程序，否则后续加载会跑旧代码 */
    if (!S_flag) pal_remove(outfile ? outfile : "a.osimg");

    if (S_flag) {
        int rc = dump_asm(src, outfile ? outfile : "out.asm");
        pal_free(heap_img); pal_free(owned);
        return rc;
    }

    /* 主路径：一次调用完成 词法->语法->代码生成->汇编->发射 */
    static uint8_t img[CLI_IMG_MAX];        /* 必须是宿主内存，且不能放栈上 */
    comp_result_t r = compiler_build(src, img, (uint32_t)sizeof img);

    /* 输出缓冲不足是可恢复的：按 required 再分配一次重试。64 KB 静态缓冲实测
     * 远够用，但一旦不够就属于"编译器内部估算"问题，不该当成编译失败报给
     * 用户 —— 这里自愈，用户无感。 */
    if (!r.ok && r.fail_kind == COMP_FAIL_RESOURCE &&
        r.required > (int)sizeof img) {
        heap_img = (uint8_t*)pal_malloc((uint32_t)r.required);
        if (heap_img) r = compiler_build(src, heap_img, (uint32_t)r.required);
    }

    if (!r.ok) {
        /* 缓冲不够时 r.required 告诉我们需要多大；CLI 选择直接报错，
         * 因为 64 KB 已经远超实测需求（180 符号 -> 508 B）。 */
        /* 退出码刻意区分：源码错误表示「去改代码」(1)；编译器自身受限
         * 表示「环境不够、不是代码问题」(2)；内部不一致表示「这是编译器
         * 的 bug」(3)。脚本与自动化需要分辨这三种；把它们合并成一条
         * 消息正是最初的失误。 */
        switch (r.fail_kind) {
        case COMP_FAIL_SOURCE:
            if (r.err_line > 0) pal_printf("error (line %d): %s\n", r.err_line, r.err_msg);
            else                pal_printf("error: %s\n", r.err_msg);
            pal_free(heap_img); pal_free(owned);
            return 1;
        case COMP_FAIL_RESOURCE:
            pal_printf("compiler limit: %s\n", r.err_msg);
            pal_free(heap_img); pal_free(owned);
            return 2;
        case COMP_FAIL_INTERNAL:
            pal_printf("internal error: %s\n", r.err_msg);
            pal_free(heap_img); pal_free(owned);
            return 3;
        default:
            pal_printf("error: %s\n", r.err_msg);
            pal_free(heap_img); pal_free(owned);
            return 1;
        }
    }

    const char* path = outfile ? outfile : "a.osimg";
    const uint8_t* out_bytes = heap_img ? heap_img : img;
    if (pal_write_file(path, out_bytes, (uint32_t)r.code_len) != 0) {
        pal_printf("write failed: %s\n", path);
        pal_free(heap_img); pal_free(owned);
        return 1;
    }

    pal_printf("OK: %s (%d bytes code, pools: 1B=%d 2B=%d 4B=%d, frame=%d)\n",
               path, r.code_len, r.pool1, r.pool2, r.pool4, r.max_frame);
    pal_free(heap_img); pal_free(owned);
    return 0;
}