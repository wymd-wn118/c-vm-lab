/******************************************************************************
 * 词法分析器：源码文本 -> token 流。
 * 支持类型关键字 int/float/char/short/str、控制流 if/else/while/do/for、
 * 返回/打印/输入、标识符、整数/浮点/字符/字符串字面量，以及比较/逻辑/复合
 * 运算符，跳过空白、// 行注释与块注释（斜杠星号对）。
 ******************************************************************************/
#include "lexer.h"
#include "error.h"   /* 字符串字面量超限时报诊断（绝不静默截断） */

/*
 * @brief  判断字符是否为十进制数字。
 * @param  int c   待检测字符（按 int 传入，兼容 EOF/-1）
 * @return int     非 0 表示是数字（'0'~'9'），0 表示不是
 * @note   仅识别数字本身，不处理正负号与小数点，后者由数字分支另行处理。 */
static int is_digit(int c) { return c >= '0' && c <= '9'; }

/*
 * @brief  判断字符是否为标识符首/组成字符（大小写字母或下划线）。
 * @param  int c   待检测字符
 * @return int     非 0 表示是字母/下划线，0 表示不是
 * @note   不含数字；数字由 is_digit 处理，组合判断统一走 is_alnum。 */
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

/*
 * @brief  判断字符是否为标识符组成字符（字母/下划线/数字）。
 * @param  int c   待检测字符
 * @return int     非 0 表示是标识符字符，0 表示不是
 * @note   词法扫描标识符主循环的条件判断依赖此函数。 */
static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }

/*
 * @brief  关键字匹配：将标识符文本映射到对应的词法类别。
 * @param  const char* s   已收集的标识符/关键字文本（以 '\0' 结尾）
 * @return tok_kind_t      命中返回对应 TK_* 关键字类别；否则返回 TK_ID
 * @note   线性比对 15 个关键字；parser 后续主要依据返回的 kind 区分关键字
 *         与普通标识符，文本本身经 sp_add 进全局池供 sp_get 取回。 */
static tok_kind_t kw(const char* s) {
    if (!pal_strcmp(s, "int"))    return TK_INT;
    if (!pal_strcmp(s, "float"))  return TK_FLOAT;
    if (!pal_strcmp(s, "char"))   return TK_CHAR;
    if (!pal_strcmp(s, "short"))  return TK_SHORT;
    if (!pal_strcmp(s, "str"))    return TK_STR;
    if (!pal_strcmp(s, "struct")) return TK_STRUCT;
    if (!pal_strcmp(s, "return")) return TK_RETURN;
    if (!pal_strcmp(s, "print"))  return TK_PRINT;
    if (!pal_strcmp(s, "func"))   return TK_FUNC;
    if (!pal_strcmp(s, "main"))   return TK_MAIN;
    if (!pal_strcmp(s, "if"))     return TK_IF;
    if (!pal_strcmp(s, "else"))   return TK_ELSE;
    if (!pal_strcmp(s, "while"))  return TK_WHILE;
    if (!pal_strcmp(s, "do"))     return TK_DO;
    if (!pal_strcmp(s, "for"))    return TK_FOR;
    if (!pal_strcmp(s, "input"))  return TK_INPUT;
    return TK_ID;
}

/* token 容量估算：源码里平均每 ~2 字节出一个 token 是保守上界（单字符运算符就占 1 字节），
 * 再留 8 个余量给末尾 TK_EOF 与对齐误差。
 * 历史实现固定 8192 个，即便只有 14 个 token 的源码也占 8.4 MB——在 STM32F4（192 KB SRAM）
 * 上完全不可行，故改为按需估算。 */
#define LEX_MIN_TOKENS 64u
#define LEX_MAX_TOKENS 65535u

/*
 * @brief  词法分析核心：将源码文本切分为 token 流。
 * @param  const char* src   以 '\0' 结尾的源码字符串
 * @param  int* out_n        输出参数，返回 token 总数（含末尾 TK_EOF）
 * @return token_t*          动态分配的 token 数组（调用方 comp_free）；
 *                           失败返回 NULL 且 *out_n 置 0
 * @note   容量按源码长度估算（slen/2+8）并支持翻倍扩容；字符串字面量与标识符
 *         经 sp_add 进全局字符串池，token 仅存 str_off 偏移。在整个编译器流水线
 *         （lex→parse→codegen→assemble→emit）中本函数是第一道工序，必须在
 *         sp_init 之后调用、sp_free 之前全程使用池。 */
token_t* lex(const char* src, int* out_n) {
    *out_n = 0;
    if (!src) return NULL;

    size_t slen = pal_strlen(src);
    /* 实验：先给小容量，让下面的 2 倍增长路径去扩。
     * 旧的估算 (slen/2 + 8) 在声明密集的源码上多分配约 76%，
     * 白占 100 KB 堆里的几十 KB。 */
    uint32_t cap = LEX_MIN_TOKENS;      /* 保守估算 */
    if (cap < LEX_MIN_TOKENS) cap = LEX_MIN_TOKENS; /* 至少够放最小程序 */
    if (cap > LEX_MAX_TOKENS) cap = LEX_MAX_TOKENS; /* 防 size_t→uint32 溢出与异常输入 */

    token_t* toks = (token_t*)comp_malloc(sizeof(token_t) * cap);
    if (!toks) return NULL;                          /* MCU 上内存不足：显式失败，不静默越界 */
    int n = 0;
    int line = 1;
    const char* p = src;
    while (*p) {
        /* 容量守护：估算偏低时按 2 倍扩容；彻底失败则收尾并报告已产出的部分。 */
        if (n + 2 > (int)cap) {
            uint32_t ncap = (cap > LEX_MAX_TOKENS / 2u) ? LEX_MAX_TOKENS : cap * 2u;
            if (ncap == cap) break;                  /* 已到上限，停止扫描 */
            token_t* nt = (token_t*)comp_realloc(toks, sizeof(token_t) * cap, sizeof(token_t) * ncap);
            if (!nt) break;                          /* 扩容失败：保留已扫描部分 */
            toks = nt; cap = ncap;
        }
        if (*p == '\n') { line++; p++; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\r') { p++; continue; }
        /* 注释 */
        if (*p == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') line++; p++; }
            if (*p) {
                p += 2;
            } else {
                /* 到文件尾都没等到闭合分隔符。静默处理会把剩余内容
                 * 全当注释吞掉，还照样产出一个「合法」镜像 —— 必须报错。 */
                comp_error(line, COMP_ERR_SYNTAX, "unterminated block comment");
            }
            continue;
        }
        /* 字符串字面量 "..." （支持 \" 与 \\） */
        if (*p == '"') {
            p++;
            /* 临时 buf 仅用于逐字符收集；字符串池化后长度不再受限，
             * 但 buf 仍是栈上固定缓冲，超 TK_STR_MAX 的部分仅丢弃进 buf（继续扫描），
             * 最后统一 sp_add 进池（池能容纳完整长度）。ival = 实际字符数。 */
            char buf[TK_STR_MAX + 1]; int i = 0;
            int over = 0;
            int total = 0;
            while (*p && *p != '"') {
                int c;
                if (*p == '\\' && p[1]) {
                    p++;
                    char e = *p++;
                    if      (e == 'n') c = '\n';
                    else if (e == 't') c = '\t';
                    else if (e == '\\') c = '\\';
                    else if (e == '"')  c = '"';
                    else                c = (unsigned char)e;
                } else {
                    c = (unsigned char)*p++;
                }
                total++;
                if (i < TK_STR_MAX) buf[i++] = (char)c;
                else over = 1;          /* 继续扫完字面量，但 buf 不再存 */
            }
            if (*p == '"') {
                p++;
            } else {
                /* 未闭合的字符串字面量：不报的话，`s = "abc` 会一直吃到
                 * 文件尾，并静默变成字符串 "abc"。 */
                comp_error(line, COMP_ERR_SYNTAX, "unterminated string literal");
            }
            buf[i] = '\0';
            token_t* t = &toks[n++];
            t->line = line; t->kind = TK_STRING;
            t->ival = total;                        /* 实际字符数 */
            t->str_off = sp_add(buf);               /* 池化：进全局字符串池 */
            if (over) {
                comp_error(line, COMP_ERR_SYNTAX,
                           "string literal exceeds %d chars in lexer buffer", TK_STR_MAX);
            }
            continue;
        }
        /* 字符字面量 'x' */
        if (*p == '\'') {
            p++;
            int c = (unsigned char)*p;
            if (*p == '\\' && p[1]) {
                p++;
                char cc = *p++;
                if (cc == 'n') c = '\n';
                else if (cc == 't') c = '\t';
                else if (cc == '\\') c = '\\';
                else if (cc == '\'') c = '\'';
                else c = (unsigned char)cc;
            } else if (*p) p++;
            if (*p == '\'') p++;
            token_t* t = &toks[n++];
            t->line = line; t->kind = TK_NUM; t->ival = c;   /* 字符作为 int 常量 */
            continue;
        }
        /* 数字（含浮点） */
        if (is_digit(*p) || (*p == '.' && is_digit(p[1]))) {
            int isf = 0; char buf[32]; int i = 0;
            int over = 0;                       /* 数字过长标志 */
            /* 三处收集循环都加 i<31 保护，防止超长数字越过 buf[32] 写坏栈。
             * 超限时仍继续消费字符（只丢弃不写入），避免剩余数字被当成新 token。 */
            while (is_digit(*p)) { if (i < 31) buf[i++] = *p; else over = 1; p++; }
            if (*p == '.') {
                isf = 1;
                if (i < 31) buf[i++] = '.'; else over = 1;
                p++;
                while (is_digit(*p)) { if (i < 31) buf[i++] = *p; else over = 1; p++; }
            }
            if (*p == 'e' || *p == 'E') {
                isf = 1;
                if (i < 31) buf[i++] = *p; else over = 1;
                p++;
                if (*p == '+' || *p == '-') { if (i < 31) buf[i++] = *p; else over = 1; p++; }
                while (is_digit(*p)) { if (i < 31) buf[i++] = *p; else over = 1; p++; }
            }
            buf[i] = 0;
            if (over) comp_error(line, COMP_ERR_SYNTAX,
                                 "number too long (max 31 chars)");
            token_t* t = &toks[n++];
            t->line = line;
            if (isf) { t->kind = TK_FNUM; t->fval = (float)pal_atof(buf); }
            else     { t->kind = TK_NUM;  t->ival = pal_atoi(buf); }
            continue;
        }
        /* 标识符 / 关键字 */
        if (is_alpha(*p)) {
            char buf[32]; int i = 0;
            while (is_alnum(*p) && i < 31) buf[i++] = *p++;
            buf[i] = 0;
            token_t* t = &toks[n++];
            t->line = line;
            t->kind = kw(buf);
            /* 只有标识符（TK_ID）入池，关键字不再入池：
             * 关键字的语义完全由 kind 表达，其名字从不通过 sp_get 取用，
             * 入池纯属浪费（实测 bench_fib 中关键字占池 63%）。
             * 关键字 token 的 str_off 置 0，sp_get(0) 返回空串，语义即"无字符串"。
             * 注意：超 31 字符的标识符仍截断入池并报错；关键字最长仅 6 字符，
             * 不会触发该分支，但吞字符逻辑对所有情况保留以防死循环。 */
            if (i >= 31 && is_alnum(*p)) {
                while (is_alnum(*p)) p++;   /* 吞掉剩余，避免死循环 */
                if (t->kind == TK_ID)
                    comp_error(line, COMP_ERR_SYNTAX,
                               "identifier too long (max 31 chars)");
            }
            t->str_off = (t->kind == TK_ID) ? sp_add(buf) : 0;
            continue;
        }
        /* 运算符 / 分界符（注意多字符优先） */
        token_t* t = &toks[n++]; t->line = line;
        switch (*p) {
            case '(': t->kind = TK_LPAREN; p++; break;
            case ')': t->kind = TK_RPAREN; p++; break;
            case '{': t->kind = TK_LBRACE;  p++; break;
            case '}': t->kind = TK_RBRACE;  p++; break;
            case '[': t->kind = TK_LBRACKET; p++; break;
            case ']': t->kind = TK_RBRACKET; p++; break;
            case ';': t->kind = TK_SEMI;    p++; break;
            case ',': t->kind = TK_COMMA;   p++; break;
            case '.': t->kind = TK_DOT;     p++; break;
            case '+':
                if (p[1] == '+') { t->kind = TK_INC; p += 2; }
                else if (p[1] == '=') { t->kind = TK_PLUSEQ; p += 2; }
                else { t->kind = TK_PLUS; p++; }
                break;
            case '-': t->kind = TK_MINUS; p++; break;
            case '*': t->kind = TK_STAR; p++; break;
            case '/': t->kind = TK_SLASH; p++; break;
            case '%': t->kind = TK_MOD; p++; break;
            case '=':
                if (p[1] == '=') { t->kind = TK_EQ; p += 2; }
                else { t->kind = TK_ASSIGN; p++; }
                break;
            case '<':
                if (p[1] == '=') { t->kind = TK_LE; p += 2; }
                else { t->kind = TK_LT; p++; }
                break;
            case '>':
                if (p[1] == '=') { t->kind = TK_GE; p += 2; }
                else { t->kind = TK_GT; p++; }
                break;
            case '!':
                if (p[1] == '=') { t->kind = TK_NE; p += 2; }
                else { t->kind = TK_NOT; p++; }
                break;
            case '&':
                if (p[1] == '&') { t->kind = TK_AND; p += 2; }
                else {
                    /* 单个 '&'：本语言不支持，若不撤销槽位就会留下一个
                       kind 未初始化的 token（读 comp_malloc 残留值），
                       导致 parser 误解后续代码却静默产出残缺镜像。 */
                    comp_error(line, COMP_ERR_SYNTAX,
                               "unexpected '&' (did you mean '&&'?)");
                    p++; n--;
                }
                break;
            case '|':
                if (p[1] == '|') { t->kind = TK_OR; p += 2; }
                else {
                    comp_error(line, COMP_ERR_SYNTAX,
                               "unexpected '|' (did you mean '||'?)");
                    p++; n--;
                }
                break;
            default:
                /* 未知字符：同样必须撤销槽位（n--），否则留下未初始化 kind 的 token。 */
                comp_error(line, COMP_ERR_SYNTAX, "unexpected character");
                p++; n--;
                break;
        }
    }
    toks[n].kind = TK_EOF; toks[n].line = line;
    *out_n = n + 1;
    return toks;
}
