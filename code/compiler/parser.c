/******************************************************************************
 * 语法分析器（递归下降）。支持的类 C 脚本语言：
 *   program   := (struct_def | func_def | global_decl)*
 *                顶层成员「顺序不限」：每类由开头 token 唯一识别，可任意交错出现
 *                （代码用 if/else if 分派，分支顺序只是实现细节，不影响语义）
 *   struct_def := 'struct' ID '{' (field ';')* '}' ';'      结构体定义，见 parse_struct_def()
 *   field     := (type | ID) ID                             字段：内置类型，或嵌套结构体
 *                                                           注：作类型的 ID 必须是「已定义的结构体名」
 *                                                           （靠 is_struct_name 查表消歧）；
 *                                                           函数名等不是类型，不能作字段类型
 *   func_def  := 'func' ('main'|ID) '(' params? ')' '{' stmt* '}'
 *   global_decl := type ID ('[' NUM ']')? ('=' expr)? ';'   全局变量声明（含数组/初值）
 *                | ID ID ';'                                结构体全局变量（结构体名+变量名）
 *   params    := type ID (',' type ID)*                     形参：类型可省略，默认 int
 *   type      := 'int' | 'float' | 'char' | 'short' | 'str' 类型关键字（见 parse_type）
 *   stmt      := type ID ('['NUM']')? ('=' expr)? ';'       声明/全局
 *              | ID ('++' | '+=' expr | '=' expr) ';'       赋值/自增/复合
 *              | 'print' '(' expr ')' ';'
 *              | 'input' ID ';'
 *              | 'return' expr? ';'
 *              | 'if' '(' expr ')' stmt ('else' stmt)?
 *              | 'while' '(' expr ')' stmt
 *              | 'do' stmt 'while' '(' expr ')' ';'
 *              | 'for' '(' stmt expr? ';' expr? ')' stmt
 *              | '{' stmt* '}'                               块
 *              | expr ';'                                    表达式语句(函数调用)
 *   expr      := lor
 *   lor       := land ('||' land)*
 *   land      := cmp ('&&' cmp)*
 *   cmp       := add ('<'|'>'|'<='|'>='|'=='|'!=' add)*
 *   add       := mul (('+'|'-') mul)*
 *   mul       := una (('*'|'/'|'%') una)*
 *   una       := ('-'|'!') una | postfix
 *   postfix   := primary ( '(' args ')'   |  '[' expr ']'  |  '.' ID  |  '++' )*
 *                四种后缀同属一层，按「出现顺序」自左向右结合，可链式组合；
 *                实现见 parse_postfix()（第一段取 primary，第二段后缀循环）。
 *   primary   := NUM | FNUM | STRING | ID
 *              | '(' type ')' postfix                        类型转换(cast)
 *              | '(' expr ')'
 *   args      := (expr (',' expr)*)?                         实参：可为空，逗号分隔，无尾随逗号
 * 注：变量/表达式的精确类型由 codegen 依据符号表解析；本阶段仅构建名称与结构。
 *     历史遗留：global_decl / type / args 三个非终结符曾被引用而未定义，
 *     现已依据 parse() / parse_type() / parse_postfix() 的实际实现补全。
 ******************************************************************************/
#include "parser.h"
#include "error.h"

/* 解析游标：pos 为"当前待处理 token"的下标，T 为 lexer 产出的 token 数组。
 * 二者均为静态全局，故本解析器不可重入（同一时刻只能解析一个文件）。 */
static int       pos;
static token_t*  T;

/* 表达式递归解析深度上限（防止深嵌套撑爆调用栈）。
 * 依据：每嵌套一层（括号 / 函数调用实参 / 数组下标）都要重新走一遍完整
 * 优先级链，约消耗 8 个栈帧（lor→land→cmp→add→mul→una→post→primary）。
 * 实测未加保护时约 3000 层括号即触发 STATUS_STACK_OVERFLOW（进程崩溃、
 * 无任何错误提示）。正常源码极少超过 10 层，故 64 已留足余量；
 * 若目标平台栈很小（如 MCU 的 8 KB），可将此值下调至 16~32。 */
#define PARSE_MAX_DEPTH 64

/* 当前表达式递归深度：进入 parse_expr 时 +1，退出时 -1。
 * 与 pos / T 同为静态全局，故解析器同样不可重入。 */
static int g_pdepth = 0;

/* 结构体定义注册表：记录已定义的结构体，供 is_struct_name 查询。
 * 与 prog_t->structs 持有同一批 struct_def_t 指针，故释放时只能释放一次
 * （见 free_prog：仅通过 p->structs 释放，本表只读不释放）。 */
static struct_def_t* P_SDEFS[64]; static int P_NSDEF = 0;

/*
 * @brief  查询某个名字是否为已定义的结构体类型名。
 * @param  const char* nm   待查询的名字（普通 C 字符串，通常以 '\0' 结尾）
 * @return int              1=是已定义的结构体类型，0=不是
 * @note   线性扫描 P_SDEFS（结构体数量很少，代价可忽略）。存在的原因是消解
 *         "Point p;" 这类 ID ID 序列的歧义——结构体类型名在词法上就是标识符，
 *         不像 int/float 有专门关键字，只能查表才能与"两个并列变量"区分。 */
static int is_struct_name(const char* nm) {
    for (int i = 0; i < P_NSDEF; i++) if (!pal_strcmp(sp_get(P_SDEFS[i]->name_off), nm)) return 1;
    return 0;
}

/*
 * @brief  取当前 token 的指针（不移动游标）。
 * @param  无（无参数）
 * @return token_t*  指向 T[pos] 的指针
 * @note   返回的是数组元素地址而非拷贝：adv() 之后该指针仍指向原 token，
 *         因此"先 t = cur() 存下、再 adv()、随后继续使用 t 的字段"是安全用法。 */
static token_t* cur(void)  { return &T[pos]; }

/*
 * @brief  游标前进一格（消费/跳过当前 token）。
 * @param  无（无参数）
 * @return 无（void）
 * @note   最简的"跳过"操作；常与 cur() 配合使用。 */
static void      adv(void)  { pos++; }

/*
 * @brief  匹配并消费：当前 token 为指定类型则前进。
 * @param  tok_kind_t k  期望的 token 类型
 * @return int           1=匹配且已前进，0=不匹配（游标保持不动）
 * @note   与 peek 的唯一区别就是"是否消费"：peek 只看不动，match 吃掉。
 *         用于消费语法上应当出现的 token（如分号、右括号）。 */
static int       match(tok_kind_t k) { if (cur()->kind == k) { adv(); return 1; } return 0; }

/* 期待一个「语法上必需」的记号并跳过它。与 match() 不同，
 * 不匹配就是语法错误。旧代码在约 48 处本该表示「必须有」的地方用了 match()，
 * 于是缺 ';' 或 ')' 的错误源码被静默接受，还照样产出可运行镜像。 */
static int       expect(tok_kind_t k, const char* what)
{
    if (cur()->kind == k) { adv(); return 1; }
    comp_error(cur()->line, COMP_ERR_SYNTAX, "expected %s", what);
    return 0;
}

/* for 头部里的步进子句后面跟的是 ')' 而不是 ';'，
 * 所以语句结束符在这里必须是可选的。parse_stmt() 被 init 与 step
 * 子句复用，因此用一个标志位区分，而不是再写一条解析路径。 */
static int       g_for_step = 0;

static void      stmt_end(void)
{
    if (g_for_step) return;
    /* 这里必须用 expect()，不能用 stmt_end()：后者会递归 */
    expect(TK_SEMI, "';'");
}

/*
 * @brief  试探：当前 token 是否为指定类型（不移动游标）。
 * @param  tok_kind_t k  待判断的 token 类型
 * @return int           1=是，0=否
 * @note   用于分支选择（如判断 ID 后是否紧跟 '(' 以识别函数调用），
 *         绝不消费 token，因此可安全地"先看再决定"。 */
static int       peek(tok_kind_t k) { return cur()->kind == k; }

/* token 总数：parse() 入口保存，供 at_end() 做长度边界判断。 */
static int       g_ntok = 0;

/*
 * @brief  统一的「是否已到达 token 流末尾」判断。
 * @param  无（无参数）
 * @return int  1=已到末尾，0=未到
 * @note   EOF 哨兵与长度边界「双保险」：正常由 lexer 保证末尾写入 TK_EOF，
 *         循环靠它终止；但若 token 数组异常（无 EOF / 被截断），用 g_ntok
 *         兜底，避免 cur() 越界读 T[pos]。
 *         所有遍历型循环都应使用它——原先只有 parse() 判了长度，其余三处
 *         仅判 EOF，防御不一致，故收敛到本函数统一处理。 */
static int at_end(void) {
    return pos >= g_ntok || peek(TK_EOF);
}

/*
 * @brief  解析并消费一个类型关键字，返回对应的变量类型。
 * @param  无（无参数）
 * @return vtype_t  T_INT/T_FLOAT/T_CHAR/T_SHORT/T_STR 之一
 * @note   非类型关键字时返回 T_INT 兜底（不消费、不报错），由调用方保证前置判断。
 *         结构体类型不在此处理——它由 is_struct_name 判断（结构体名是标识符）。 */
static vtype_t parse_type(void) {
    tok_kind_t k = cur()->kind;
    if (k == TK_INT)   { adv(); return T_INT; }
    if (k == TK_FLOAT) { adv(); return T_FLOAT; }
    if (k == TK_CHAR)  { adv(); return T_CHAR; }
    if (k == TK_SHORT) { adv(); return T_SHORT; }
    if (k == TK_STR)   { adv(); return T_STR; }
    return T_INT;
}

/*
 * @brief  判断当前 token 是否为类型关键字（不消费）。
 * @param  无（无参数）
 * @return int  1=是类型关键字，0=不是
 * @note   用于区分 "int x;"（声明）与 "x = 1;"（赋值）——两者都以标识符形态
 *         出现，只能靠开头是否为类型关键字来分流。 */
static int is_type_kw(void) {
    tok_kind_t k = cur()->kind;
    return k == TK_INT || k == TK_FLOAT || k == TK_CHAR || k == TK_SHORT || k == TK_STR;
}

/* 前向声明：parse_block/parse_stmt 互相需要先见到对方，
 * 且语句解析会用到表达式解析，故在此提前声明。 */
static expr_t* parse_expr(void);
static stmt_t* parse_stmt(void);

/* ---- 节点构造（集中判空）----
 * 分配可能失败（MM 耗尽）。每个构造点都必须检查返回值；
 * 把检查集中在一处，既让 37 个调用点都老实检查，
 * 又把「本会发生的 NULL 解引用」变成一条正常的诊断。
 * 它们在 OOM 时仍返回 NULL —— 调用方必须检查 —— 但错误已经报过，
 * 所以调用点只需 `if (!x) return NULL;`。 */
static expr_t* new_expr(void) {
    /* 这里必须直接调 comp_calloc()：调 new_expr() 会无限递归，
     * 直接把栈打爆。 */
    expr_t* e = (expr_t*)comp_calloc(1, sizeof(expr_t));   /* 用 comp_calloc，不是 new_expr() */
    if (!e) comp_error(0, COMP_ERR_INTERNAL, "out of memory (expr node)");
    return e;
}
static stmt_t* new_stmt(void) {
    stmt_t* s = (stmt_t*)comp_calloc(1, sizeof(stmt_t));   /* 同理：用 comp_calloc，不是 new_stmt() */
    if (!s) comp_error(0, COMP_ERR_INTERNAL, "out of memory (stmt node)");
    return s;
}

/* ---- 表达式 ---- */

/*
 * @brief  构造一个变量引用节点（E_VAR）。
 * @param  const char* nm  变量名（普通 C 字符串）
 * @return expr_t*         新建的 E_VAR 节点
 * @note   名字经 sp_add 入全局字符串池；若池中已有同名串（去重），
 *         会直接复用其偏移而不新增副本。初始类型按 T_INT，精确类型由 codegen
 *         依据符号表解析。 */
static expr_t* mkvar(const char* nm) {
    expr_t* e = new_expr();
    if (!e) return NULL;
    e->kind = E_VAR; e->type = T_INT; e->name_off = sp_add(nm);
    return e;
}

/*
 * @brief  生成一个占位表达式节点（E_NUM，值 0）。
 * @param  无（无参数）
 * @return expr_t*  已清零并置为 E_NUM 的节点
 * @note   用途：语法错误处「本该有表达式却缺失」时填一个非空占位，
 *         避免把 NULL 传给 codegen 造成空指针崩溃（实测 "x;" 这类语句
 *         会让编译器以 0xC0000005 访问违规退出，且因缓冲未刷新连错误
 *         信息都看不到）。错误本身已由 comp_error 报出，这里只是让
 *         解析继续走完（错误恢复），使一次编译能报出多处问题。 */
static expr_t* mk_dummy_expr(void) {
    expr_t* e = new_expr();
    if (!e) return NULL;
    e->kind = E_NUM; e->type = T_INT; e->ival = 0;
    return e;
}

/* 前置声明：错误恢复路径需要把"读完即丢弃"的子树/语句显式释放，避免内存泄漏。
 * 这两个函数定义在文件末尾的「AST 释放」区，此处需提前声明。 */
static void free_expr_list(expr_t* e);
static void free_stmt_list(stmt_t* s);

/*
 * @brief  解析一个表达式并立即释放（错误恢复专用）。
 * @param  无（无参数）
 * @return 无（void）
 * @note   用于「语法已出错、但要读完 token 让游标继续前进」的场合：
 *         表达式必须完整解析（否则游标错位引发级联报错），但结果已经没用，
 *         若不释放就泄漏整棵子树，故解析完立刻归还。 */
static void parse_and_discard_expr(void) {
    free_expr_list(parse_expr());
}

/*
 * @brief  解析后缀表达式（优先级最高的一层）：基本单元 + 其后可跟的任意个后缀。
 * @param  无（无参数）
 * @return expr_t*  基本单元节点，或被若干层后缀节点包裹的表达式
 * @note   本函数对应标准的 postfix-expression，分两段：
 *
 *         【第一段】取一个「基本单元」（primary）：
 *           TK_NUM    -> E_NUM   （ival 存整数值）
 *           TK_FNUM   -> E_FNUM  （fval 存浮点值）
 *           TK_STRING -> E_STR   （name_off 复用 token 偏移；ival 存字符数）
 *           TK_ID     -> E_VAR   （仅造变量节点，后缀交给第二段）
 *           TK_LPAREN -> E_CAST（后跟类型关键字）或普通括号表达式
 *           其他      -> 报错并返回 E_NUM 占位节点（保证解析能继续）
 *
 *         【第二段】后缀循环：统一处理 '('调用 / '['下标 / '.'字段 / '++'后增。
 *         四种后缀同属一层，按「出现顺序」自左向右结合（与 C 一致），
 *         因此 a[0]++ 解析为 (a[0])++。
 *         用循环而非递归：栈深度恒定，超长链式（a.b.c...）不会爆栈。
 *
 *         每种后缀都设了「base 合法性」守卫：codegen 只能处理有限几种
 *         组合（例如它靠 E_IDX 的 l->name_off 查数组符号），因此这里
 *         对无法正确生成代码的组合明确报错，绝不静默产出错误程序。
 *
 *         关键：函数调用的 name_off 只是"名字引用"，不展开被调函数的树，
 *         所以递归调用（fib 内再调 fib）才能正常建树而不会无限展开。 */
static expr_t* parse_postfix(void) {
    token_t* t = cur();
    if (t->kind == TK_NUM) {
        adv(); expr_t* e = new_expr();
        if (!e) return NULL;
        e->kind = E_NUM; e->type = T_INT; e->ival = t->ival; return e;
    }
    if (t->kind == TK_FNUM) {
        adv(); expr_t* e = new_expr();
        if (!e) return NULL;
        e->kind = E_FNUM; e->type = T_FLOAT; e->fval = t->fval; return e;
    }
    if (t->kind == TK_STRING) {
        adv(); expr_t* e = new_expr();
        if (!e) return NULL;
        e->kind = E_STR; e->type = T_STR;
        e->ival = t->ival; e->name_off = t->str_off;   /* 复用 name_off 存字面量在池中的偏移 */
        return e;
    }
    if (t->kind == TK_ID) {
        adv(); expr_t* e = mkvar(sp_get(t->str_off));

        /* ---- 第二段：后缀循环 ----
         * 四种后缀（'('调用 / '['下标 / '.'字段 / '++'后增）同属一层，
         * 按「出现顺序」自左向右结合，故 a[0]++ 解析为 (a[0])++。
         * 每处理完一个后缀就回到这里继续检查下一个，因此后缀可以链式组合。
         * 用循环而非递归：栈深度恒定，超长链式（a.b.c.d...）不会爆栈。 */
        for (;;) {
            /* ① 函数调用 f(...) */
            if (peek(TK_LPAREN)) {
                /* E_CALL 用 name_off 存"被调函数名"、l 存实参链，没有字段能存放
                 * "被调者表达式"，故要求 e 仍是简单的 E_VAR（此前未包过后缀）。
                 * 像 a.b(1) / f(1)(2) 这类"被调者本身是表达式"的写法，本结构
                 * 无法表达，明确报错而不是静默生成错树。 */
                if (e->kind != E_VAR) {
                    comp_error(cur()->line, COMP_ERR_SYNTAX, "函数调用的被调者只能是函数名");
                    adv();                      /* 吃掉 '('，保证游标继续前进 */
                    if (!peek(TK_RPAREN)) {
                        for (;;) {
                            parse_and_discard_expr();   /* 读完实参并释放，避免游标错位 */
                            if (!peek(TK_COMMA)) break;
                            adv();
                            if (peek(TK_RPAREN)) break;
                        }
                    }
                    expect(TK_RPAREN, "')'");
                    continue;                   /* 不建 E_CALL，e 保持原样 */
                }
                adv();                          /* 吃掉 '(' */
                e->kind = E_CALL;               /* 复用 E_VAR 节点改标签（name_off 共用） */
                expr_t* head = NULL, * tail = NULL;
                if (!peek(TK_RPAREN)) {         /* 非空实参表才解析 */
                    for (;;) {
                        expr_t* a = parse_expr();
                        if (!head) head = tail = a; else { tail->next = a; tail = a; }
                        if (!peek(TK_COMMA)) break;
                        adv();
                        /* 吃掉逗号后若紧跟 ')'，说明是尾随逗号（如 f(1,)）：
                         * 明确报错并退出循环。否则下一轮 parse_expr() 会把 ')' 当成
                         * 表达式开头、落入兜底分支消费掉它，只给出含糊的
                         * "无法识别的表达式"，且后续 match(TK_RPAREN) 会失配。 */
                        if (peek(TK_RPAREN)) {
                            comp_error(t->line, COMP_ERR_SYNTAX, "实参列表不允许尾随逗号");
                            break;
                        }
                    }
                }
                expect(TK_RPAREN, "')'");
                e->l = head;                    /* 实参链挂到 l */
                continue;                       /* 回到循环，继续检查下一个后缀 */
            }
            /* ② 数组下标 a[i] */
            if (peek(TK_LBRACKET)) {
                adv();                          /* 吃掉 '[' */
                /* codegen 的 case E_IDX 直接用 e->l->name_off 查数组符号，
                 * 即 base 必须是「数组变量」；像 a.b[i] 这种 base 是字段的写法
                 * 会取到空名字、静默生成错误代码，故在此明确拦下。 */
                if (e->kind != E_VAR) {
                    comp_error(cur()->line, COMP_ERR_SYNTAX, "暂不支持：下标只能用于数组变量");
                    parse_and_discard_expr();   /* 读完下标表达式并释放，避免游标错位 */
                    match(TK_RBRACKET);
                    continue;
                }
                /* 单层下标 a[i]：复用 e 节点改 kind，并用 mkvar 重建 base
                 * （codegen 依赖 E_IDX 的 l->name_off 查数组符号，必须保持）。 */
                e->arr_sz = -1;                 /* 标记为下标访问（具体大小在 codegen 确定） */
                e->kind = E_IDX;
                e->l = mkvar(sp_get(t->str_off));   /* 基址 */
                e->r = parse_expr();                /* 下标表达式 */
                match(TK_RBRACKET);
                continue;
            }

            /* ③ 结构体字段访问 a.b.c */
            if (peek(TK_DOT)) {
                /* base 合法性：codegen 的 resolve_dot 只能从「变量 / 字段链」出发计算地址，
                 * 即 base 必须是 E_VAR 或 E_DOT。若 base 是 E_CALL / E_IDX 等，
                 * 会把它当地址乱解引用（实测 f(5).x 能编过但运行输出垃圾值），
                 * 因此明确报错，绝不静默产出语义错误的程序。 */
                if (e->kind != E_VAR && e->kind != E_DOT) {
                    comp_error(cur()->line, COMP_ERR_SYNTAX, "暂不支持：只能对变量或结构体字段取成员");
                    break;
                }
                adv();                          /* 吃掉 '.' */
                if (cur()->kind != TK_ID) { comp_error(cur()->line, COMP_ERR_SYNTAX, "field name expected"); break; }
                expr_t* d = new_expr();
                if (!d) return NULL;
                d->kind = E_DOT; d->type = T_INT;
                d->field_off = cur()->str_off; adv();
                d->l = e; e = d;                /* 向外包裹，支持 a.b.c 链式 */
                continue;
            }

            /* ④ 后增 x++ */
            if (peek(TK_INC)) {
                /* 自增要求操作数是「可写回的左值」：变量 / 数组元素 / 字段。
                 * 函数调用返回值、字面量等无处写回，codegen 无法处理，
                 * 故在此拦下（例如 f(1)++ 、5++）。 */
                if (e->kind != E_VAR && e->kind != E_IDX && e->kind != E_DOT) {
                    comp_error(cur()->line, COMP_ERR_SYNTAX, "暂不支持：++ 只能用于变量、数组元素或字段");
                    adv();                      /* 吃掉 '++'，保证游标前进 */
                    continue;
                }
                adv();
                expr_t* u = new_expr();
                if (!u) return NULL;
                u->kind = E_UN; u->type = T_INT; u->uop = 2; u->l = e;   /* uop=2 = 后增 */
                e = u;
                continue;
            }

            break;                              /* 没有更多后缀，收工 */
        }
        return e;
    }
    if (t->kind == TK_LPAREN) {
        adv();
        if (is_type_kw()) {                    /* 强转（cast） */
            expr_t* e = new_expr();
            if (!e) return NULL;
            e->kind = E_CAST; e->ctype = parse_type();
            e->type = e->ctype;
            expect(TK_RPAREN, "')'");
            e->l = parse_postfix();            /* 紧随的后缀表达式；(int)x 足够覆盖 */

            return e;
        }
        expr_t* e = parse_expr();
        expect(TK_RPAREN, "')'");
        return e;
    }
    adv();
    comp_error(t->line, COMP_ERR_SYNTAX, "无法识别的表达式");
    expr_t* e = new_expr();
    if (!e) return NULL;
    e->kind = E_NUM; e->type = T_INT; return e;
}

/*
 * @brief  解析一元表达式：前缀 '-'（负号）或 '!'（逻辑非）。
 * @param  无（无参数）
 * @return expr_t*  E_UN 节点（uop=0 负号 / uop=1 逻辑非），无前缀时回退 parse_post
 * @note   递归调用自身，因此支持 "--x"、"!!x" 这类连续一元运算。 */
static expr_t* parse_una(void) {
    /* 连续前缀运算符（----x / !!x）会递归调用自身。本函数不经由 parse_expr，
     * 因此 parse_expr 处的保护覆盖不到这条路径，必须在此独立设防。
     * 注意：只在「确有前缀、会递归」的分支计数，普通路径不计数，
     *       避免与 parse_expr 的计数叠加而把合法代码的深度限额压低一半。 */
    if (peek(TK_MINUS) || peek(TK_NOT)) {
        if (g_pdepth >= PARSE_MAX_DEPTH) {
            comp_error(cur()->line, COMP_ERR_SYNTAX, "表达式嵌套过深");
            expr_t* e = new_expr();
            if (!e) return NULL;
            e->kind = E_NUM; e->type = T_INT;
            return e;
        }
        int is_not = peek(TK_NOT);      /* 先记录运算符种类，adv() 后再取就读不到了 */
        adv();                          /* 吃掉 '-' 或 '!' */
        g_pdepth++;                     /* 仅在此递归分支计数 */
        expr_t* operand = parse_una();
        g_pdepth--;
        expr_t* u = new_expr();
        if (!u) return NULL;
        u->kind = E_UN; u->type = T_INT;
        u->uop = is_not ? 1 : 0;        /* 0=负号，1=逻辑非 */
        u->l = operand;
        return u;
    }
    return parse_postfix();
}

/*
 * @brief  解析乘法级表达式：'*' '/' '%'。
 * @param  无（无参数）
 * @return expr_t*  左结合的 E_BIN 链，或单个操作数（没有运算符时）
 * @note   用 for(;;) 循环实现左结合：a/b/c 解析为 (a/b)/c。
 *         类型按"任一操作数为 T_FLOAT 则结果为 T_FLOAT"提升。 */
static expr_t* parse_mul(void) {
    expr_t* l = parse_una();
    for (;;) {
        tok_kind_t k = cur()->kind;
        if (k != TK_STAR && k != TK_SLASH && k != TK_MOD) break;
        binop_t op = (k == TK_STAR) ? OP_MUL : (k == TK_SLASH) ? OP_DIV : OP_MOD;
        adv();
        expr_t* r = parse_una();
        expr_t* e = new_expr();
        if (!e) return NULL;
        e->kind = E_BIN; e->op = op; e->l = l; e->r = r;
        e->type = (l->type == T_FLOAT || r->type == T_FLOAT) ? T_FLOAT : T_INT;
        l = e;
    }
    return l;
}

/*
 * @brief  解析加法级表达式：'+' '-'。
 * @param  无（无参数）
 * @return expr_t*  左结合的 E_BIN 链，或单个操作数（没有运算符时）
 * @note   ?优先级的实现关键：本层调用 parse_mul，因此 '*' 的节点必然挂在
 *         '+' 之下——运算符优先级不是查表算出来的，而是由"谁调用谁"的
 *         函数层次天然保证的。这也是整个递归下降最核心的设计。 */
static expr_t* parse_add(void) {
    expr_t* l = parse_mul();
    for (;;) {
        tok_kind_t k = cur()->kind;
        if (k != TK_PLUS && k != TK_MINUS) break;
        binop_t op = (k == TK_PLUS) ? OP_ADD : OP_SUB;
        adv();
        expr_t* r = parse_mul();
        expr_t* e = new_expr();
        if (!e) return NULL;
        e->kind = E_BIN; e->op = op; e->l = l; e->r = r;
        e->type = (l->type == T_FLOAT || r->type == T_FLOAT) ? T_FLOAT : T_INT;
        l = e;
    }
    return l;
}

/*
 * @brief  解析比较表达式：'<' '>' '<=' '>=' '==' '!='。
 * @param  无（无参数）
 * @return expr_t*  左结合的 E_BIN 链，或单个操作数（没有比较符时）
 * @note   比较结果固定为 T_INT（本语言把布尔当作整数使用）。 */
static expr_t* parse_cmp(void) {
    expr_t* l = parse_add();
    for (;;) {
        tok_kind_t k = cur()->kind;
        binop_t op;
        switch (k) {
            case TK_LT: op = OP_LT; break;
            case TK_GT: op = OP_GT; break;
            case TK_LE: op = OP_LE; break;
            case TK_GE: op = OP_GE; break;
            case TK_EQ: op = OP_EQ; break;
            case TK_NE: op = OP_NE; break;
            default: return l;
        }
        adv();
        expr_t* r = parse_add();
        expr_t* e = new_expr();
        if (!e) return NULL;
        e->kind = E_BIN; e->op = op; e->l = l; e->r = r; e->type = T_INT;  /* 比较结果=布尔 */
        l = e;
    }
}

/*
 * @brief  解析逻辑与表达式：'&&'。
 * @param  无（无参数）
 * @return expr_t*  左结合的 E_BIN(OP_AND) 链，或单个操作数（没有 && 时）
 * @note   结果固定为 T_INT。 */
static expr_t* parse_land(void) {
    expr_t* l = parse_cmp();
    while (peek(TK_AND)) {
        adv();
        expr_t* r = parse_cmp();
        expr_t* e = new_expr();
        if (!e) return NULL;
        e->kind = E_BIN; e->op = OP_AND; e->l = l; e->r = r; e->type = T_INT;
        l = e;
    }
    return l;
}

/*
 * @brief  解析逻辑或表达式：'||'（优先级最低的二元运算）。
 * @param  无（无参数）
 * @return expr_t*  左结合的 E_BIN(OP_OR) 链，或单个操作数（没有 || 时）
 * @note   结果固定为 T_INT。 */
static expr_t* parse_lor(void) {
    expr_t* l = parse_land();
    while (peek(TK_OR)) {
        adv();
        expr_t* r = parse_land();
        expr_t* e = new_expr();
        if (!e) return NULL;
        e->kind = E_BIN; e->op = OP_OR; e->l = l; e->r = r; e->type = T_INT;
        l = e;
    }
    return l;
}

/*
 * @brief  解析表达式的入口（优先级链的最外层）。
 * @param  无（无参数）
 * @return expr_t*  表达式树的根节点
 * @note   直接转调 parse_lor。整条优先级链为
 *           调用链：expr -> lor -> land -> cmp -> add -> mul -> una -> post -> primary
 *         从外到内优先级递增，故"树越深的运算符越先被求值"。 */
static expr_t* parse_expr(void) {
    if (comp_oom()) return NULL;   /* 之前已分配失败：直接停下，别往 NULL 上跑 */

    /* 递归深度保护：括号嵌套 / 函数调用实参 / 数组下标都会经由本函数重新
     * 走一遍完整优先级链（每层约 8 个栈帧）。若不限制，上千层括号就会
     * 撑爆调用栈（实测约 3000 层即 STATUS_STACK_OVERFLOW，进程直接崩溃
     * 且无任何错误提示）。这里超限即报错并返回占位节点，让编译继续。 */
    if (g_pdepth >= PARSE_MAX_DEPTH) {
        comp_error(cur()->line, COMP_ERR_SYNTAX, "表达式嵌套过深");
        /* 返回占位节点而非 NULL：调用方一律按非 NULL 处理，避免空指针；
         * 解析得以继续（错误恢复），而不是让栈溢出把进程打死。 */
        expr_t* e = new_expr();
        if (!e) return NULL;
        e->kind = E_NUM; e->type = T_INT;
        return e;
    }
    g_pdepth++;                    /* 进入一层表达式 */
    expr_t* e = parse_lor();
    g_pdepth--;                    /* 退出时归还，保证后续解析不受影响 */
    return e;
}

/* ---- 语句 ---- */
/*
 * @brief  解析语句块 '{' stmt* '}'。
 * @param  无（无参数）
 * @return stmt_t*  S_BLOCK 节点，body 指向块内第一条语句（其余由 next 串成链）
 * @note   用 head/tail 双指针建链表（本文件建链表的标准写法，另见于实参链、
 *         函数体）。循环条件同时检查 '}' 与 EOF，避免未闭合的 '{' 造成死循环。 */
static stmt_t* parse_block(void) {
    expect(TK_LBRACE, "'{'");
    stmt_t* head = NULL, * tail = NULL;
    while (!at_end() && !peek(TK_RBRACE)) {
        stmt_t* s = parse_stmt();
        /* 必须在这里 break：parse_stmt() 在 OOM 时返回 NULL 且【不消费任何
         * token】，若不跳出，下一轮循环条件依然成立 —— 实测 80 个 if（约 500
         * 个 AST 节点）把 100 KB 堆撑爆后，parser 就在这个循环里 100% CPU
         * 空转。跳出后 expect('}') 会失败，而 compiler.c 的 comp_oom() 检查
         * 会给出准确的 "out of memory" 诊断。 */
        if (!s) break;
        if (!head) head = tail = s; else { tail->next = s; tail = s; }
    }
    expect(TK_RBRACE, "'}'");
    stmt_t* b = new_stmt();
    if (!b) return NULL;
    b->kind = S_BLOCK; b->body = head;
    return b;
}

/*
 * @brief  解析"单条语句或语句块"。
 * @param  无（无参数）
 * @return stmt_t*  单条语句节点，或 S_BLOCK 节点
 * @note   用于 if / while / for 的分支体，使其同时支持两种写法：
 *         "if (x) y = 1;"（单语句）与 "if (x) { ... }"（块）。 */
static stmt_t* parse_stmt_or_block(void) {
    if (peek(TK_LBRACE)) return parse_block();
    return parse_stmt();
}

/*
 * @brief  解析一条语句（语句层的大分派函数）。
 * @param  无（无参数）
 * @return stmt_t*  对应类型的语句节点，见 @note
 * @note   按"开头 token"分派，共 11 种语句：
 *           类型关键字(int/float/...) -> S_DECL   变量声明（可带数组、初值）
 *           ID 且为结构体名           -> S_DECL   结构体变量声明（is_struct=1）
 *           ID 且后跟 '.'             -> S_ASSIGN 结构体字段赋值（lhs_expr）
 *           ID                        -> S_ASSIGN 赋值/自增
 *           print -> S_PRINT      input -> S_INPUT      return -> S_RETURN
 *           if -> S_IF（cond+body+els）        while -> S_WHILE（cond+body）
 *           do  -> S_DOWHILE（先 body 后 cond）
 *           for -> S_FOR（init_s+cond+step_s+body）
 *           '{' -> S_BLOCK       其他 -> S_EXPR（表达式语句，如函数调用）
 *         "=" / "+=" / "++" 三种形态共用 S_ASSIGN，仅靠 compound 标志（0/1/2）
 *         区分——用标志位避免语句类型爆炸。全程"报错但继续"，以一次报出多个错误。 */
static stmt_t* parse_stmt(void) {
    if (comp_oom()) return NULL;   /* 之前已分配失败：直接停下，别往 NULL 上跑 */

    token_t* t = cur();
    if (is_type_kw()) {
        vtype_t ty = parse_type();
        stmt_t* s = new_stmt();
        if (!s) return NULL;
        s->kind = S_DECL; s->type = ty;
        if (cur()->kind == TK_ID) { s->name_off = cur()->str_off; adv(); }
        else { comp_error(t->line, COMP_ERR_SYNTAX, "声明缺少变量名"); }
        if (peek(TK_LBRACKET)) {            /* 数组 int a[10]; */
            adv();
            if (cur()->kind == TK_NUM) { s->arr_sz = cur()->ival; adv(); }
            else { comp_error(t->line, COMP_ERR_SYNTAX, "数组大小必须为整数"); s->arr_sz = 0; }
            match(TK_RBRACKET);
        }
        if (match(TK_ASSIGN)) {
            /* 数组带初值：本语言不支持——没有数组字面量语法（见 expr_kind_t，
             * 无数组字面量节点），若放行会在 codegen 阶段被静默跳过：既不 ALLOC
             * 也不赋值，导致句柄恒为 0、运行时写坏堆边界（实测 ERR=6）。
             * 故在此明确报错，与「结构体字面量不支持」同在 parser 层拦下，尽早反馈。
             * 仍调用 parse_expr() 消费掉初值表达式，避免游标错位引发级联报错。 */
            if (s->arr_sz > 0) {
                comp_error(t->line, COMP_ERR_SYNTAX, "数组不支持初始化，请逐元素赋值");
            }
            s->init = parse_expr();
        }
        stmt_end();
        return s;
    }
    if (t->kind == TK_ID) {
        /* 结构体类型声明：Point p; */
        if (is_struct_name(sp_get(t->str_off)) && T[pos + 1].kind == TK_ID) {
            uint32_t sname_off = t->str_off; adv();
            stmt_t* s = new_stmt();
            if (!s) return NULL;
            s->kind = S_DECL; s->type = T_STRUCT; s->is_struct = 1;
            s->sname_off = sname_off;
            if (cur()->kind == TK_ID) { s->name_off = cur()->str_off; adv(); }
            else comp_error(t->line, COMP_ERR_SYNTAX, "struct var name expected");
            if (match(TK_ASSIGN)) { comp_error(t->line, COMP_ERR_SYNTAX, "struct literal init not supported"); parse_expr(); }
            stmt_end();
            return s;
        }
        /* 结构体字段赋值：p.x = ... */
        if (T[pos + 1].kind == TK_DOT) {
            /* 保存游标：若最终发现不是赋值（例如 p.x + 1;），需回退到此处
             * 改按「表达式语句」重新解析，避免 ID 开头的表达式被误判为赋值。 */
            int save_pos = pos;
            expr_t* lhs = mkvar(sp_get(t->str_off)); adv();
            while (peek(TK_DOT)) {
                adv();
                if (cur()->kind != TK_ID) { comp_error(t->line, COMP_ERR_SYNTAX, "field name expected"); break; }
                expr_t* d = new_expr();
                if (!d) return NULL;
                d->kind = E_DOT; d->type = T_INT;
                d->field_off = cur()->str_off; adv();
                d->l = lhs; lhs = d;
            }
            stmt_t* s = new_stmt();
            if (!s) return NULL;
            s->kind = S_ASSIGN; s->lhs_expr = lhs;
            if (match(TK_ASSIGN))      { s->compound = 0; s->e = parse_expr(); }
            else if (match(TK_PLUSEQ)) { s->compound = 1; s->e = parse_expr(); }
            else if (match(TK_INC))    { s->compound = 2; }
            else {
                /* 不是赋值/自增 → 其实是「表达式语句」（如 p.x + 1; 或 p.x();）。
                 * 回退到本条语句开头，改用 S_EXPR 重新解析；
                 * 游标只是 int，保存/恢复零成本，且能自然处理任意复杂的下标/嵌套。 */
                pos = save_pos;                 /* ★ 回退 */
                free_expr_list(lhs);            /* 丢弃已建的左值树，避免泄漏 */
                comp_free(s);                    /* 丢弃 S_ASSIGN 节点 */
                stmt_t* e = new_stmt();
                if (!e) return NULL;
                e->kind = S_EXPR;
                e->e = parse_expr();
                stmt_end();
                return e;
            }
            stmt_end();
            return s;
        }
        /* 保存游标：若最终发现不是赋值（例如 f(1); 或 a + b;），需回退到此处
         * 改按「表达式语句」重新解析。必须在 adv() 之前保存，才能退回标识符。 */
        int save_pos = pos;
        uint32_t nm_off = t->str_off; adv();
        stmt_t* s = new_stmt();
        if (!s) return NULL;
        s->kind = S_ASSIGN; s->name_off = nm_off;
        if (peek(TK_LBRACKET)) {            /* 数组元素赋值 a[i] = ... */
            adv(); s->lhs_arr = 1; s->lhs_idx = parse_expr(); match(TK_RBRACKET);
        }

        if (match(TK_ASSIGN))      { s->compound = 0; s->e = parse_expr(); }
        else if (match(TK_PLUSEQ)) { s->compound = 1; s->e = parse_expr(); }
        else if (match(TK_INC))    { s->compound = 2; }
        else {
            /* 不是赋值/自增 → 其实是「表达式语句」（如 f(1); 、a + b; 、a[i];）。
             * 回退到本条语句开头，改用 S_EXPR 重新解析。
             * 注意：此前可能已为 a[i] 建过下标表达式，回退前必须释放，避免泄漏。 */
            pos = save_pos;                 /* ★ 回退 */
            free_expr_list(s->lhs_idx);     /* 丢弃已解析的下标（可能为 NULL，函数内会判断） */
            comp_free(s);                    /* 丢弃 S_ASSIGN 节点 */
            stmt_t* e = new_stmt();
            if (!e) return NULL;
            e->kind = S_EXPR;
            e->e = parse_expr();
            stmt_end();
            return e;
        }
        stmt_end();
        return s;
    }
    if (t->kind == TK_PRINT) {
        adv(); stmt_t* s = new_stmt();
        if (!s) return NULL;
        s->kind = S_PRINT; match(TK_LPAREN); s->e = parse_expr();
        expect(TK_RPAREN, "')'"); stmt_end(); return s;
    }
    if (t->kind == TK_INPUT) {
        adv(); stmt_t* s = new_stmt();
        if (!s) return NULL;
        s->kind = S_INPUT;
        if (cur()->kind == TK_LPAREN) adv();   /* 兼容 input(x) 与 input x 两种写法 */
        if (cur()->kind == TK_ID) { s->name_off = cur()->str_off; adv(); }
        else comp_error(t->line, COMP_ERR_SYNTAX, "input 后需变量名");
        if (cur()->kind == TK_RPAREN) adv();
        stmt_end(); return s;
    }
    if (t->kind == TK_RETURN) {
        adv(); stmt_t* s = new_stmt();
        if (!s) return NULL;
        s->kind = S_RETURN;
        if (!peek(TK_SEMI)) s->e = parse_expr();
        stmt_end(); return s;
    }
    if (t->kind == TK_IF) {
        adv(); stmt_t* s = new_stmt();
        if (!s) return NULL;
        s->kind = S_IF; match(TK_LPAREN); s->cond = parse_expr(); expect(TK_RPAREN, "')'");
        s->body = parse_stmt_or_block();
        if (match(TK_ELSE)) s->els = parse_stmt_or_block();
        return s;
    }
    if (t->kind == TK_WHILE) {
        adv(); stmt_t* s = new_stmt();
        if (!s) return NULL;
        s->kind = S_WHILE; match(TK_LPAREN); s->cond = parse_expr(); expect(TK_RPAREN, "')'");
        s->body = parse_stmt_or_block(); return s;
    }
    if (t->kind == TK_DO) {
        adv(); stmt_t* s = new_stmt();
        if (!s) return NULL;
        s->kind = S_DOWHILE; s->body = parse_stmt_or_block();
        match(TK_WHILE); match(TK_LPAREN); s->cond = parse_expr(); expect(TK_RPAREN, "')'");
        stmt_end(); return s;
    }
    if (t->kind == TK_FOR) {
        adv(); stmt_t* s = new_stmt();
        if (!s) return NULL;
        s->kind = S_FOR; match(TK_LPAREN);
        if (!peek(TK_SEMI)) s->init_s = parse_stmt();
        else stmt_end();
        if (!peek(TK_SEMI)) s->cond = parse_expr();
        stmt_end();
        if (!peek(TK_RPAREN)) {
            /* 步进子句以 ')' 结束，这里没有 ';' 要吃掉 */
            int save = g_for_step; g_for_step = 1;
            s->step_s = parse_stmt();
            g_for_step = save;
        }
        expect(TK_RPAREN, "')'");
        s->body = parse_stmt_or_block(); return s;
    }
    if (t->kind == TK_LBRACE) return parse_block();
    /* 表达式语句（如函数调用） */
    {
        stmt_t* s = new_stmt();
        if (!s) return NULL;
        s->kind = S_EXPR; s->e = parse_expr();
        stmt_end(); return s;
    }
}

/* ---- 函数 / 程序 ---- */
/*
 * @brief  解析函数定义 'func' ('main'|ID) '(' params? ')' '{' stmt* '}'。
 * @param  无（无参数）
 * @return func_t*  函数节点（含函数名、参数列表、函数体语句链）
 * @note   main 会被特殊标记（is_main=1），供 codegen 生成程序入口。
 *         参数类型可省略，默认 int（func fib(n) 等价 func fib(int n)）。
 *         参数名复用 token 的 str_off（零拷贝）；函数体用 head/tail 建链表挂到 body。 */
/*
 * @brief  解析函数参数列表 '(' params? ')'，结果写入函数节点 f。
 * @param  func_t* f  函数节点（参数写入 ptype / pname_off / nparam）
 * @return 无（void）
 * @note   参数类型可省略，默认 int（func fib(n) 等价 func fib(int n)）。
 *         参数名复用 token 的 str_off（零拷贝）。上限 8 个（ptype[8] 定长）。
 *         从 parse_func 中抽出，使 parse_func 的主干（函数名→参数→体）一目了然。 */
static void parse_params(func_t* f) {
    match(TK_LPAREN);
    if (!peek(TK_RPAREN)) {
        for (;;) {
            vtype_t pt = T_INT;
            if (is_type_kw()) pt = parse_type();   /* 参数类型可省略，默认 int */
            if (cur()->kind == TK_ID) {
                /* 参数个数上限：func_t 的 ptype[]/pname_off[] 为定长 8。
                 * 超限必须报错并停止登记——否则会越界写入破坏相邻内存，表现为：
                 * 9 个参数时报出误导性的「变量未声明」（第 9 个参数根本没登记进表），
                 * 20 个参数时直接访问违规崩溃（进程退出码 0xC0000005）。
                 * 这里只报错并停止登记，不再消费剩余 token：parser 没有 token 总数，
                 * 盲目循环消费有越界读风险；已报错则编译最终失败，不会产出镜像。 */
                if (f->nparam >= 8) {
                    comp_error(cur()->line, COMP_ERR_SYNTAX, "函数参数过多（最多 8 个）");
                    break;
                }
                f->ptype[f->nparam] = pt;
                f->pname_off[f->nparam] = cur()->str_off;
                f->nparam++;
                adv();
            } else { comp_error(cur()->line, COMP_ERR_SYNTAX, "参数缺名"); break; }
            if (!peek(TK_COMMA)) break;
            adv();
        }
    }
    expect(TK_RPAREN, "')'");
}

static func_t* parse_func(void) {
    match(TK_FUNC);
    func_t* f = (func_t*)comp_calloc(1, sizeof(func_t));
    if (!f) { comp_error(0, COMP_ERR_INTERNAL, "out of memory (func node)"); return NULL; }
    if (cur()->kind == TK_MAIN) { adv(); f->name_off = sp_add("main"); f->is_main = 1; }
    else if (cur()->kind == TK_ID) { f->name_off = cur()->str_off; adv(); }
    else { comp_error(cur()->line, COMP_ERR_SYNTAX, "func 后缺函数名"); }
    parse_params(f);
    expect(TK_LBRACE, "'{'");
    stmt_t* head = NULL, * tail = NULL;
    while (!at_end() && !peek(TK_RBRACE)) {
        stmt_t* s = parse_stmt();
        /* parse_stmt() 在 OOM（或无法恢复的语法错误）时返回 NULL。
         * 把 NULL 链进 body 会让 tail == NULL，下一轮就解引用 NULL。
         * 所以直接结束这个 body —— 错误已经报过，编译会干净地失败。 */
        if (!s) break;
        if (!head) head = tail = s; else { tail->next = s; tail = s; }
    }
    expect(TK_RBRACE, "'}'");
    f->body = head;
    return f;
}

/*
 * @brief  解析结构体定义 'struct' ID '{' field* '}' ';'。
 * @param  prog_t* p  程序根节点（结构体定义登记到 p->structs）
 * @return 无（void）
 * @note   字段类型支持基本类型，也支持嵌套结构体（后者由 is_struct_name 判定，
 *         并把类型名存入 fsname_off）。
 *         ?? 同一个 struct_def_t 指针会同时存入 p->structs 与全局 P_SDEFS，
 *         因此释放时只能通过 p->structs 释放一次，否则 double-free。 */
static void parse_struct_def(prog_t* p) {
    match(TK_STRUCT);
    if (cur()->kind != TK_ID) { comp_error(cur()->line, COMP_ERR_SYNTAX, "struct name expected"); return; }
    uint32_t nm_off = cur()->str_off; adv();
    struct_def_t* sd = (struct_def_t*)comp_calloc(1, sizeof(struct_def_t));
    if (!sd) { comp_error(0, COMP_ERR_INTERNAL, "out of memory (struct def)"); return; }
    sd->name_off = nm_off; sd->nfield = 0;
    if (!match(TK_LBRACE)) { comp_error(cur()->line, COMP_ERR_SYNTAX, "expected '{'"); return; }
    while (!at_end() && !peek(TK_RBRACE)) {
        /* 字段数硬上限：ftype/fname_off/fsname_off 都是定长 32 的数组，
         * 超出会越界写（破坏相邻内存，比崩溃更难查），故在此拦下。
         * 注意：这是「循环填数组」，不会增加递归深度 g_pdepth，
         *       因此递归深度保护管不到这里，必须单独设限。 */
        if (sd->nfield >= 32) {
            comp_error(cur()->line, COMP_ERR_SYNTAX, "结构体字段过多（最多 32 个）");
            break;
        }
        vtype_t ft = T_INT; uint32_t fsname_off = 0;
        if (is_type_kw()) ft = parse_type();
        else if (cur()->kind == TK_ID && is_struct_name(sp_get(cur()->str_off))) { ft = T_STRUCT; fsname_off = cur()->str_off; adv(); }
        else { comp_error(cur()->line, COMP_ERR_SYNTAX, "struct field type"); ft = T_INT; }
        if (cur()->kind == TK_ID) { sd->fname_off[sd->nfield] = cur()->str_off; adv(); }
        else { comp_error(cur()->line, COMP_ERR_SYNTAX, "field name expected"); }
        sd->ftype[sd->nfield] = ft; sd->fsname_off[sd->nfield] = fsname_off;
        sd->nfield++;
        stmt_end();
    }
    expect(TK_RBRACE, "'}'"); stmt_end();
    if (p->nstruct < 64) p->structs[p->nstruct++] = sd;
    if (P_NSDEF < 64) P_SDEFS[P_NSDEF++] = sd;
}

/*
 * @brief  语法分析入口：token 流 -> AST（递归下降）。
 * @param  token_t* toks  lexer 产出的 token 数组
 * @param  int n          token 数量（用于顶层循环的边界保护，防止越界读）
 * @return prog_t*        AST 根节点（堆分配，调用方须用 free_prog 释放）
 * @note   顶层只允许三件事：结构体定义 / 函数定义 / 全局声明；
 *         其他一律报错并 adv() 跳过（错误恢复，以便一次报出多个语法错误）。
 *         全局声明复用 parse_stmt 生成 S_DECL，用头插法挂到 globals 链
 *         （故 globals 顺序与源码相反，但不影响语义）。
 *         函数数量达到 64 上限时报"函数过多"并停止。 */
prog_t* parse(token_t* toks, int n) {
    pos = 0; T = toks;  /* 复位解析游标：pos 指向第 0 个 token，T 指向 token 数组 */
    g_ntok = n;         /* 保存 token 总数，供 at_end() 做长度边界判断 */
    g_pdepth = 0;       /* 复位递归深度，避免跨次解析时计数残留 */

    /* 建立 AST 根节点（calloc 清零，故 nfunc/nstruct 初值为 0、各指针为 NULL） */
    prog_t* p = (prog_t*)comp_calloc(1, sizeof(prog_t));
    if (!p) {
        /* 根节点都没分配出来：后面什么都做不了，
         * 报一条诊断后退出，而不是解引用 NULL。 */
        comp_error(0, COMP_ERR_INTERNAL, "out of memory (AST root)");
        sp_free();
        return NULL;
    }

    /* 顶层主循环：逐个消费「顶层成员」直到 EOF。
     * 注意：顶层是并列的成员列表，不是表达式，因此这里没有优先级链，
     *       只有「看开头一个 token 决定是哪一类成员」的简单分派。
     *
     * 结束条件统一用 at_end()：以 EOF 哨兵为主、以 g_ntok 长度为兜底，
     * 保证 token 数组异常时也不会越界读。 */
    while (!at_end()) {

        /* ① 结构体定义：struct P { int x; ... };
         *    解析结果存入 p->structs[]（定长 64），nstruct 自增 */
        if (peek(TK_STRUCT)) {
            parse_struct_def(p);
        }

        /* ② 函数定义：func f(...) { ... }
         *    存入 p->funcs[]（定长 64）；装满只能报"函数过多"并停止 */
        else if (peek(TK_FUNC)) {
            if (p->nfunc >= 64) { comp_error(cur()->line, COMP_ERR_INTERNAL, "函数过多"); break; }
            p->funcs[p->nfunc++] = parse_func();   /* nfunc 兼作下标与计数器 */
        }

        /* ③ 内置类型开头的全局声明：int g = 10;  /  int a[5];
         *    复用 parse_stmt() 产出 S_DECL，再挂到 globals 链 */
        else if (is_type_kw()) {
            stmt_t* s = parse_stmt();
            if (!s) break;   /* OOM / 不可恢复：停下来，别解引用 NULL */
            if (s->kind == S_DECL) {
                s->next = p->globals; p->globals = s;   /* 头插法：新节点插到链头 */
            } else {
                /* 非 S_DECL（如误写出表达式语句）：不进 globals，
                 * 但必须释放，否则这棵子树再无人引用 → 内存泄漏。 */
                free_stmt_list(s);
            }
        }

        /* ④ 结构体类型的全局声明：P p;
         *    开头是 ID 且该 ID 是已定义过的 struct 名，才认作类型名 */
        else if (cur()->kind == TK_ID && is_struct_name(sp_get(cur()->str_off))) {
            stmt_t* s = parse_stmt();
            if (!s) break;   /* OOM / 不可恢复：停下来，别解引用 NULL */
            if (s->kind == S_DECL) {
                s->next = p->globals; p->globals = s;   /* 同样是头插法 */
            } else {
                free_stmt_list(s);                      /* 同上：丢弃前必须释放 */
            }
        }

        /* ⑤ 以上都不是 -> 报错 + 跳过一个 token（错误恢复）
         *    不中断整个循环，这样一次编译能报出多处语法错误，
         *    而不是遇到第一个错误就退出 */
        else {
            comp_error(cur()->line, COMP_ERR_SYNTAX, "顶层仅允许全局声明或函数定义");
            adv();
        }
    }
    return p;
}

/* ===================== AST 释放 =====================
 * next 是横向链表（用迭代处理，避免长链递归爆栈）；
 * l / r 是纵向子树（递归处理）。所有节点均由 comp_calloc 分配。 */

/*
 * @brief  释放表达式链（含 l/r 子树与 next 兄弟链）。
 * @param  expr_t* e   表达式链头（可为 NULL）
 * @return 无（void）
 * @note   E_CALL 的 l 是实参链（由 next 串接），本函数会顺着 next 一并释放。 */
static void free_expr_list(expr_t* e) {
    while (e) {
        expr_t* nxt = e->next;
        free_expr_list(e->l);
        free_expr_list(e->r);
        comp_free(e);
        e = nxt;
    }
}

/*
 * @brief  释放语句链（含各表达式字段、嵌套语句体与 next 兄弟链）。
 * @param  stmt_t* s   语句链头（可为 NULL）
 * @return 无（void）
 * @note   body/els/init_s/step_s 既可能是单节点也可能是块内链表，统一按链释放。 */
static void free_stmt_list(stmt_t* s) {
    while (s) {
        stmt_t* nxt = s->next;
        free_expr_list(s->init);
        free_expr_list(s->e);
        free_expr_list(s->cond);
        free_expr_list(s->lhs_expr);
        free_expr_list(s->lhs_idx);
        free_stmt_list(s->body);
        free_stmt_list(s->els);
        free_stmt_list(s->init_s);
        free_stmt_list(s->step_s);
        comp_free(s);
        s = nxt;
    }
}

/*
 * @brief  释放整棵 AST（prog）及其所有子节点。
 * @param  prog_t* p   parse 返回的 AST 根（可为 NULL）
 * @return 无（void）
 * @note   仅释放 p->funcs / p->structs / p->globals 持有的节点；字符串本身
 *         在全局池中，由 sp_free 统一回收，本函数不触碰。 */
void free_prog(prog_t* p) {
    if (!p) return;
    for (int i = 0; i < p->nfunc; i++) {
        if (p->funcs[i]) {
            free_stmt_list(p->funcs[i]->body);
            comp_free(p->funcs[i]);
        }
    }
    for (int i = 0; i < p->nstruct; i++) comp_free(p->structs[i]);
    free_stmt_list(p->globals);
    comp_free(p);
}
