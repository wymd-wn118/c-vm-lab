/******************************************************************************
 * 语法分析器对外头文件：定义 AST（抽象语法树）的全部节点结构，
 * 以及「构建 / 销毁」两个对外函数。
 *
 * AST 分层（自顶向下）：
 *   prog_t（根：整个程序）
 *     ├── funcs[]   -> func_t       函数（并列，最多 64 个）
 *     ├── globals   -> stmt_t 链    全局变量声明
 *     └── structs[] -> struct_def_t 结构体定义（最多 64 个）
 *   func_t.body -> stmt_t 链（语句序列，靠 next 串接）
 *   stmt_t      -> 具名字段挂表达式与子语句（组成结构固定，故用具名字段）
 *   expr_t      -> l / r 递归子树（表达式可任意嵌套，故用树）
 *
 * 设计要点：
 *   1) 表达式 / 语句二分 —— 「有没有值」：有值的是表达式（递归树），
 *      无值、只执行动作的是语句（具名字段 + next 链表）
 *   2) 字符串一律存为「全局字符串池偏移」uint32_t，用 sp_get(off) 取回内容，
 *      节点内不内嵌 char[]（省内存，且池内可去重）
 *   3) 一字段多用 —— expr_t 的 l / r / next 在不同 kind 下含义不同，
 *      必须结合 kind 解读（空间优先的取舍）
 *   4) 定长上限（funcs[64] / ptype[8] / ftype[32]）—— 内存占用可预测，
 *      对 MCU 而言「不会失控」比「平均更省」更重要
 ******************************************************************************/
#ifndef PARSER_H_
#define PARSER_H_
#include "compiler.h"
#include "lexer.h"

/* 变量 / 表达式的类型。codegen 依据它选择整数指令还是浮点指令等。 */
typedef enum {
    /** 整型（int）：默认类型，4 字节；codegen 选用整数指令 */
    T_INT,
    /** 单精度浮点（float）：codegen 选用浮点指令与浮点寄存器 */
    T_FLOAT,
    /** 字符型（char）：值以字符码点形式存于 expr_t.ival */
    T_CHAR,
    /** 短整型（short） */
    T_SHORT,
    /** 字符串型（str）：实际内容在全局字符串池，节点只存偏移 */
    T_STR,
    /** 结构体型（struct）：需配合 struct_name 才能定位具体的结构体定义 */
    T_STRUCT
} vtype_t;

/* 二元运算符（E_BIN 节点的 op 字段）。分三组：算术 / 比较 / 逻辑。 */
typedef enum {
    /* ---- 算术（优先级最高，对应 parse_mul / parse_add） ---- */
    /** 加法 '+' */
    OP_ADD,
    /** 减法 '-' */
    OP_SUB,
    /** 乘法 '*' */
    OP_MUL,
    /** 除法 '/' */
    OP_DIV,
    /** 取模 '%' */
    OP_MOD,
    /* ---- 比较（结果按整数 0 / 1 参与运算） ---- */
    /** 小于 '<' */
    OP_LT,
    /** 大于 '>' */
    OP_GT,
    /** 小于等于 '<=' */
    OP_LE,
    /** 大于等于 '>=' */
    OP_GE,
    /** 等于 '==' */
    OP_EQ,
    /** 不等于 '!=' */
    OP_NE,
    /* ---- 逻辑 ---- */
    /** 逻辑与 '&&' */
    OP_AND,
    /** 逻辑或 '||' */
    OP_OR
} binop_t;

/* 表达式种类（expr_t.kind），决定该节点哪些字段有效：
 *   E_NUM / E_FNUM / E_CNUM  常量（整数 / 浮点 / 字符）
 *   E_VAR   变量引用          E_BIN  二元运算
 *   E_UN    一元运算（负号 / 逻辑非 / 后增）
 *   E_CALL  函数调用          E_CAST 类型转换
 *   E_STR   字符串字面量      E_IDX  数组元素访问   E_DOT 结构体字段访问 */
typedef enum {
    /** 整数字面量：值存 ival，type 为 T_INT */
    E_NUM,
    /** 浮点字面量：值存 fval，type 为 T_FLOAT */
    E_FNUM,
    /** 字符字面量：值以码点形式存 ival，type 为 T_CHAR */
    E_CNUM,
    /** 变量引用：名字在 name_off（sp_get 取回）；
     *  arr_sz>0 表示数组，is_struct / struct_name_off 表示结构体变量 */
    E_VAR,
    /** 二元运算：运算符在 op，左右操作数分别在 l / r */
    E_BIN,
    /** 一元运算：uop=0 负号 / 1 逻辑非 / 2 后增(x++)；操作数在 l */
    E_UN,
    /** 函数调用：被调函数名在 name_off，实参链挂 l（各实参用 next 串接）；
     *  只存名字引用，不展开被调函数体，故递归调用不会无限展开 */
    E_CALL,
    /** 强制类型转换：目标类型在 ctype（type 与之相同），待转换表达式在 l */
    E_CAST,
    /** 字符串字面量：内容在 name_off（池偏移），ival 存字符数 */
    E_STR,
    /** 数组元素访问 a[i]：基地址（数组变量）在 l，下标表达式在 r；arr_sz 置 -1 作标记 */
    E_IDX,
    /** 结构体字段访问 a.b：字段名在 field_off，基表达式在 l；
     *  连续字段（a.b.c）会包成多层本节点 */
    E_DOT
} expr_kind_t;

/* 表达式节点。
 * ?一字段多用：l / r / next 的含义随 kind 变化（见各字段注释）。这是刻意的
 *   空间优化——不为每种表达式单独建结构，而是复用同一套指针字段。
 *   代价是读代码时必须结合 kind 才能确定某个字段的含义。
 * 所有节点由 pal_calloc 分配（清零），因此未使用的字段为 0。
 * 求值顺序为「后序」：先左子树、再右子树、最后本节点。 */
typedef struct expr expr_t;
struct expr {
    expr_kind_t kind;     /* 表达式种类（判别标签，决定其余字段如何解读） */
    vtype_t     type;     /* 本表达式的结果类型（供 codegen 选指令） */
    int         ival;     /* E_NUM / E_CNUM(字符码点) */
    float       fval;     /* E_FNUM */
    uint32_t    name_off;      /* E_VAR / E_CALL 目标名（sp_get 取） */
    binop_t     op;       /* E_BIN */
    int         uop;      /* E_UN: 0=负号, 1=逻辑非 */
    vtype_t     ctype;    /* E_CAST 目标类型 */
    int         arr_sz;   /* E_VAR 若为数组：元素个数(>0) */
    expr_t*     l;        /* E_BIN.left / E_UN.operand / E_CAST.operand / E_CALL.arglist / E_IDX.base */
    expr_t*     r;        /* E_BIN.right / E_IDX.index */
    expr_t*     next;     /* 调用实参链 */
    uint32_t    field_off;     /* E_DOT 字段名（sp_get 取） */
    int         is_struct;/* E_VAR 是否结构体变量 */
    uint32_t    struct_name_off; /* E_VAR 结构体类型名（sp_get 取） */
};

/* 语句种类（stmt_t.kind）：
 *   S_DECL 声明  S_ASSIGN 赋值/自增  S_PRINT 打印  S_INPUT 输入
 *   S_RETURN 返回  S_IF 条件  S_WHILE / S_DOWHILE / S_FOR 循环
 *   S_EXPR 表达式语句（如函数调用）  S_BLOCK 语句块 */
typedef enum {
    /** 变量声明：类型 type / 名字 name_off / 数组大小 arr_sz / 初值 init；
     *  结构体变量时 is_struct=1 且类型名在 sname_off */
    S_DECL,
    /** 赋值或自增：目标名 name_off，右值在 e；
     *  compound 区分形态：0='='  1='+='  2='++'(后增)；
     *  左值为数组元素时 lhs_arr=1 且下标在 lhs_idx，为结构体字段时左值在 lhs_expr */
    S_ASSIGN,
    /** 打印语句 print(...)：待打印表达式在 e */
    S_PRINT,
    /** 函数返回 return ...：返回值表达式在 e（无返回值时为 NULL） */
    S_RETURN,
    /** 条件分支 if：条件 cond / then 分支 body / else 分支 els（可为 NULL） */
    S_IF,
    /** 当型循环 while：条件 cond，循环体 body */
    S_WHILE,
    /** 直到型循环 do...while：循环体 body，条件 cond（注意先执行后判断） */
    S_DOWHILE,
    /** 计数循环 for：初始化 init_s / 条件 cond / 步进 step_s / 循环体 body */
    S_FOR,
    /** 输入语句 input x：目标变量在 name_off（表达式字段 e 保留备用） */
    S_INPUT,
    /** 表达式语句：单独成句的表达式（典型为函数调用 f();），表达式在 e */
    S_EXPR,
    /** 语句块 { ... }：块内语句链在 body（各语句用 next 串接） */
    S_BLOCK
} stmt_kind_t;

/* 语句节点。
 * 与表达式不同，语句的组成部分是「固定」的（如 if 恒为 cond+body+els），
 * 因此用具名字段而非左右子树；只有「语句序列」用 next 链表串接。
 * 三种赋值形态（= / += / ++）共用 S_ASSIGN，仅靠 compound 标志区分，
 * 以避免语句类型爆炸。 */
typedef struct stmt stmt_t;
struct stmt {
    stmt_kind_t kind;     /* 语句种类（决定其余字段如何解读） */
    vtype_t     type;     /* S_DECL 类型 */
    uint32_t    name_off; /* S_DECL / S_ASSIGN / S_INPUT 变量名（sp_get 取） */
    int         arr_sz;   /* S_DECL 数组大小(>0=数组) */
    expr_t*     init;     /* S_DECL 初值 */
    expr_t*     e;        /* S_ASSIGN 右值 / S_PRINT / S_RETURN / S_INPUT / S_EXPR */
    expr_t*     cond;     /* S_IF / S_WHILE / S_DOWHILE / S_FOR 条件 */
    stmt_t*     body;     /* then / 循环体 */
    stmt_t*     els;      /* S_IF else */
    stmt_t*     init_s;   /* S_FOR 初始化 */
    stmt_t*     step_s;   /* S_FOR 步进 */
    int         compound; /* S_ASSIGN: 0='=' 1='+=' 2='++'(后增) */
    int         lhs_arr;  /* S_ASSIGN: 左值为数组元素(a[i]=...) */
    expr_t*     lhs_idx;  /* S_ASSIGN: 数组下标表达式 */
    int         is_struct;/* S_DECL: 是否为结构体声明 */
    uint32_t    sname_off;/* S_DECL: 结构体类型名（sp_get 取） */
    expr_t*     lhs_expr; /* S_ASSIGN: 左值为结构体字段(a.b=...) */
    stmt_t*     next;
};

/* 结构体定义（struct P { int x; Inner in; }）。
 * 三个数组下标 i 一一对应第 i 个字段。定长 32 项：无论实际几个字段都占
 * 相同空间（约 392 字节/结构体），是「用固定预留换简单与可预测」的取舍。
 * ?? 同一 struct_def_t 指针会被 prog_t->structs 与 parser 内部全局 P_SDEFS
 *    同时引用，释放时只能通过 prog_t->structs 释放一次（见 free_prog）。 */
typedef struct {
    uint32_t    name_off;      /* 结构体类型名（如 "P"，sp_get 取） */
    int         nfield;        /* 实际字段数（<=32） */
    vtype_t     ftype[32];     /* 各字段类型；为 T_STRUCT 表示嵌套结构体 */
    uint32_t    fname_off[32];   /* 字段名（sp_get 取） */
    uint32_t    fsname_off[32];  /* 嵌套结构体字段的类型名(ftype==T_STRUCT 时有效) */
} struct_def_t;

/* 函数节点。函数之间彼此并列（本语言不支持嵌套函数），
 * 故由 prog_t.funcs[] 数组持有，而非互相嵌套。 */
typedef struct {
    uint32_t    name_off;        /* 函数名（sp_get 取） */
    vtype_t     ptype[8];        /* 各参数类型（最多 8 个） */
    uint32_t    pname_off[8];     /* 各参数名（sp_get 取） */
    int         nparam;          /* 实际参数个数 */
    stmt_t*     body;            /* 函数体：语句链（由 next 串接） */
    int         is_main;         /* 是否为 main（codegen 据此生成程序入口） */
} func_t;

/* AST 根节点：代表整个程序，是 parse() 的返回值、codegen() 的输入。
 * 持有三类成员：函数（定长数组）、全局变量（链表）、结构体定义（定长数组）。
 * 由 free_prog() 递归释放。 */
typedef struct {
    func_t*  funcs[64];          /* 函数表（最多 64 个，超出报"函数过多"） */
    int      nfunc;              /* 实际函数数 */
    stmt_t*  globals;            /* 全局变量声明链表（头插法，顺序与源码相反） */
    struct_def_t* structs[64];   /* 结构体定义表（最多 64 个） */
    int      nstruct;            /* 实际结构体数 */
} prog_t;

/* 语法分析：tokens -> AST（递归下降）。返回动态分配的 prog_t。
 * n 为 token 总数，供顶层循环做边界保护（token 数组若无 EOF 时防止越界读）。 */
prog_t* parse(token_t* toks, int n);

/* 释放 AST：递归归还 prog 及其全部 func/struct/stmt/expr 子节点内存。
 * 调用时机：codegen 产出汇编文本之后（此后 AST 不再被访问）。
 * 注意：struct_def_t 同时被 p->structs 与全局 P_SDEFS 引用，此处仅通过
 *       p->structs 释放一次，P_SDEFS 仅作只读指针副本，不再重复释放。 */
void free_prog(prog_t* p);

#endif /* PARSER_H_ */
