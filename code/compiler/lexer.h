#ifndef LEXER_H_
#define LEXER_H_
#include "compiler.h"
#include "strpool.h"   /* token 字符串统一进全局池，token_t 只存偏移 */

/* Token 种类（词法分析的输出标签，parser 全程靠它分派）。
 *
 * 枚举顺序即下方分组顺序，lexer 的关键字表、parser 的 is_type_kw() 等判定
 * 均依赖该顺序，**不要随意调整成员次序**，否则需同步修改 lexer.c / parser.c。
 *
 * 各成员与 token_t 字段的对应关系（见下方 token_t）：
 *   只用 kind+line      ：全部运算符、分隔符、TK_EOF、各关键字
 *   额外用 ival         ：TK_NUM（整数值）、TK_STRING（字符数）
 *   额外用 fval         ：TK_FNUM（浮点值）
 *   额外用 str_off      ：TK_ID / TK_STRING / 关键字（字符串池偏移，sp_get 取回）
 */
typedef enum {
    /* ---------- 文件结束 ---------- */
    /** 输入结束标记；parser 主循环以它为终止条件 */
    TK_EOF,

    /* ---------- 类型关键字（声明变量 / 形参 / 返回值 / 强转用） ---------- */
    /** 'int'     整型 */
    TK_INT,
    /** 'float'   单精度浮点型 */
    TK_FLOAT,
    /** 'char'    字符型 */
    TK_CHAR,
    /** 'short'   短整型 */
    TK_SHORT,
    /** 'str'     字符串型 */
    TK_STR,
    /** 'struct'  结构体定义 / 结构体变量 */
    TK_STRUCT,

    /* ---------- 语句与控制关键字 ---------- */
    /** 'return'  函数返回 */
    TK_RETURN,
    /** 'print'   打印（本语言内置语句） */
    TK_PRINT,
    /** 'func'    函数定义起始 */
    TK_FUNC,
    /** 'main'    主函数名 */
    TK_MAIN,
    /** 'if'      条件分支 */
    TK_IF,
    /** 'else'    分支的否则部分 */
    TK_ELSE,
    /** 'while'   当型循环 */
    TK_WHILE,
    /** 'do'      直到型循环 do...while */
    TK_DO,
    /** 'for'     计数循环 */
    TK_FOR,
    /** 'input'   输入（本语言内置语句） */
    TK_INPUT,

    /* ---------- 标识符与字面量 ---------- */
    /** 标识符：变量名 / 函数名 / 结构体名，文本在 str_off */
    TK_ID,
    /** 整数字面量，值存 ival */
    TK_NUM,
    /** 浮点字面量，值存 fval */
    TK_FNUM,
    /** 字符串字面量，文本在 str_off，ival 复用为字符数 */
    TK_STRING,

    /* ---------- 分隔符 ---------- */
    /** '('  表达式分组 / 形参表 / 实参表 / 强制类型转换 */
    TK_LPAREN,
    /** ')' */
    TK_RPAREN,
    /** '{'  函数体 / 语句块 / 结构体定义 */
    TK_LBRACE,
    /** '}' */
    TK_RBRACE,
    /** '['  数组声明 / 数组下标访问 */
    TK_LBRACKET,
    /** ']' */
    TK_RBRACKET,
    /** ';'  语句结束 */
    TK_SEMI,
    /** '='  赋值 */
    TK_ASSIGN,
    /** ','  分隔形参 / 实参 / 同类型声明 */
    TK_COMMA,
    /** '.'  结构体成员访问（a.b.c） */
    TK_DOT,

    /* ---------- 算术运算符 ---------- */
    /** '+'  加法（亦作一元正号，parser 中按二元处理） */
    TK_PLUS,
    /** '-'  减法；作前缀时为负号（见 parse_una） */
    TK_MINUS,
    /** '*'  乘法 */
    TK_STAR,
    /** '/'  除法 */
    TK_SLASH,
    /** '%'  取模 */
    TK_MOD,
    /** '+=' 复合赋值（加法赋值） */
    TK_PLUSEQ,
    /** '++' 自增（parser 中作后缀 x++，见 parse_post） */
    TK_INC,

    /* ---------- 比较运算符（结果按整数 0/1 参与运算） ---------- */
    /** '<'  小于 */
    TK_LT,
    /** '>'  大于 */
    TK_GT,
    /** '<=' 小于等于 */
    TK_LE,
    /** '>=' 大于等于 */
    TK_GE,
    /** '==' 等于 */
    TK_EQ,
    /** '!=' 不等于 */
    TK_NE,

    /* ---------- 逻辑运算符 ---------- */
    /** '&&' 逻辑与 */
    TK_AND,
    /** '||' 逻辑或 */
    TK_OR,
    /** '!'  逻辑非（前缀，见 parse_una） */
    TK_NOT
} tok_kind_t;

/* 词法层字符串扫描临时缓冲上限（仅用于 lexer 内部逐字符收集字面量时的栈缓冲）。
 * 字符串池化后不再有 31 字符硬限制——扫描完直接 sp_add 进全局池，长度任意。
 * 此宏只为限制 lexer 内部栈上临时 sbuf 的大小，防止极端长串爆栈；超出部分
 * 仍会被 sp_add 完整追加进池（lexer.c 中超限只是停止写入临时缓冲，但继续扫
 * 描并在末尾 sp_add 已收集内容 + 剩余长度，保证池内字符串完整）。 */
#define TK_STR_MAX 256

typedef struct {
    tok_kind_t kind;
    int         line;
    int         ival;      /* TK_NUM 的整数值；TK_STRING 时为字面量长度（= pal_strlen(sp_get(str_off))） */
    float       fval;      /* TK_FNUM */
    uint32_t    str_off;   /* TK_ID/TK_STRING/关键字：全局字符串池中的偏移；
                              用 sp_get(str_off) 取内容；空串/无串为 0 */
} token_t;

/* 词法分析：src -> tokens。返回动态分配的 token 数组（调用方 free），*out_n 为数量。
 * 容量按源码长度估算（不再固定 8192），失败返回 NULL 并置 *out_n = 0 。 */
token_t* lex(const char* src, int* out_n);

#endif /* LEXER_H_ */
