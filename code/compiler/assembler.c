/******************************************************************************
 * 汇编器：VM 文本汇编 -> 字节流。
 *
 * 语法：每行一条指令，逗号或空格分隔操作数；`name:` 定义标签；`;`/`#` 起注释。
 * 寄存器写作 R0..R32；立即数支持十进制与 0x 十六进制；分支操作数的标签名
 * 解析为绝对字节地址（与 VM 的 PC=Imm 语义一致）。
 * 字节序与 VM 取指一致：立即数小端（低字节在前）——x86/F407(Cortex-M4) 均为小端，
 ******************************************************************************/
#include "assembler.h"

typedef struct { const char* mnem; int op; const char* fmt; } op_t;
static const op_t OPS[] = {
    {"MOV",1,"RR"}, {"MOV8",2,"RB"}, {"MOV16",3,"RH"}, {"MOV32",4,"RI"},
    {"LDR",5,"RRI"}, {"STR",6,"RRI"}, {"LDRX",50,"RRI"}, {"STRX",51,"RRI"},
    {"ADD",7,"RRR"}, {"ADDI",8,"RRI"}, {"SUB",9,"RRR"}, {"SUBI",10,"RRI"},
    {"MUL",11,"RRR"}, {"MULF",12,"RRR"}, {"DIV",13,"RRR"}, {"DIVF",14,"RRR"},
    {"MOD",15,"RRR"}, {"SHL",17,"RRR"}, {"SHR",18,"RRR"}, {"NOT",19,"RR"},
    {"AND",20,"RRR"}, {"OR",21,"RRR"}, {"XOR",22,"RRR"},
    {"B",23,"R"}, {"BR",24,"R"}, {"BL",25,"R"}, {"BI",26,"I"},
    {"BIR",27,"I"}, {"BIL",28,"I"},
    {"BEQI",29,"I"}, {"BNEI",30,"I"}, {"BGTI",31,"I"}, {"BGEI",32,"I"},
    {"BLTI",33,"I"}, {"BLEI",34,"I"},
    {"CMP",35,"RR"}, {"CMP8",36,"RB"}, {"CMPF",37,"RR"}, {"CMPIF",38,"RI"},
    {"PUSH",39,"R"}, {"POP",40,"R"},
    {"ALLOC",46,"RBI"}, {"FREE",47,"R"},
    {"CALL",41,"RI"}, {"RET",42,""},
    {"ADDF",43,"RRR"}, {"SUBF",44,"RRR"},
    {"SHLI",45,"RRI"}, {"SHRI",48,"RRI"},
    {"SYSCALL",49,"I"}, {"HALT",0,""},
    {"LDRXB",52,"RRI"}, {"STRXB",53,"RRI"},
    {"LDRXH",54,"RRI"}, {"STRXH",55,"RRI"},
    {"I2F",56,"RR"}, {"F2I",57,"RR"},
    {"LDRH",58,"RRR"}, {"STRH",59,"RRR"},
    {NULL,0,NULL}
};

static const op_t* find_op(const char* m) {
    for (int i = 0; OPS[i].mnem; i++)
        if (!pal_strcmp(OPS[i].mnem, m)) return &OPS[i];
    return NULL;
}

/* 去掉行内注释（; 或 # 起），返回静态副本 */
static char* strip_comment(const char* line) {
    char* s = comp_strdup(line);
    if (!s) return NULL;   /* 已 OOM：调用方本身就会检查 NULL 返回 */
    if (!s) return NULL;   /* 内存不足：必须在函数内检查，否则下面的 *p 会解引用 NULL */
    for (char* p = s; *p; p++) if (*p == ';' || *p == '#') { *p = 0; break; }
    return s;
}
static void trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    int L = (int)pal_strlen(s);
    while (L > 0 && (s[L-1] == ' ' || s[L-1] == '\t' || s[L-1] == '\r' || s[L-1] == '\n')) s[--L] = 0;
}

static int parse_reg(const char* t) {
    if ((t[0] == 'R' || t[0] == 'r') && t[1]) return pal_atoi(t + 1);
    return -1;
}
static int parse_imm(const char* t) {
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) return (int)pal_strtoul(t, NULL, 16);
    /* 支持浮点位模式后缀 u（如 1069547520u），按无符号解析避免大数溢出 */
    size_t n = pal_strlen(t);
    if (n > 0 && (t[n - 1] == 'u' || t[n - 1] == 'U')) {
        char* tmp = comp_strdup(t);
        if (!tmp) return 0;              /* 内存不足：退回 0，绝不解引用 NULL */
        tmp[n - 1] = 0;
        int v = (int)pal_strtoul(tmp, NULL, 10);
        comp_free(tmp); return v;
    }
    return (int)pal_strtol(t, NULL, 10);
}
static int is_label_tok(const char* t) { return pal_isalpha((unsigned char)t[0]) != 0; }

/*
 * @brief  按 '\n' 把汇编文本拆成行数组（每行是独立的 comp_malloc 副本）。
 * @param  const char* text  待拆分的汇编文本
 * @param  int* n            输出参数，返回行数
 * @return char**            行指针数组（调用方负责逐行 + 数组本身 comp_free）；
 *                           任一步失败返回 NULL 且 *n 置 0
 * @note   分配/扩容失败时回滚并释放本函数已分配的全部资源，绝不带"半截数组"
 *         返回——否则调用方既会泄漏又会解引用 NULL。realloc 失败时原块仍有效，
 *         故用临时指针 na 接住，失败才能正确 comp_free(arr)。 */
static char** split_lines(const char* text, int* n) {
    int cap = 256; int c = 0;
    char** arr = (char**)comp_malloc(sizeof(char*) * (uint32_t)cap);
    if (!arr) { *n = 0; return NULL; }               /* 行指针数组本身分配失败 */
    const char* p = text; const char* s = p;
    while (1) {
        if (*p == '\n' || *p == 0) {
            int len = (int)(p - s);
            char* line = (char*)comp_malloc((uint32_t)len + 1u);
            if (!line) {                             /* 单行分配失败：回滚 */
                for (int k = 0; k < c; k++) comp_free(arr[k]);
                comp_free(arr); *n = 0; return NULL;
            }
            pal_memcpy(line, s, (uint32_t)len); line[len] = 0;
            if (c >= cap) {                          /* 容量不足：翻倍扩容 */
                int ncap = cap * 2;
                char** na = (char**)comp_realloc(arr, sizeof(char*) * (uint32_t)cap, sizeof(char*) * (uint32_t)ncap);
                if (!na) {                           /* 扩容失败：先释放本次 line，再回滚旧数组 */
                    comp_free(line);
                    for (int k = 0; k < c; k++) comp_free(arr[k]);
                    comp_free(arr); *n = 0; return NULL;
                }
                arr = na; cap = ncap;
            }
            arr[c++] = line;
            s = p + 1;
            if (*p == 0) break;
        }
        p++;
    }
    *n = c;
    return arr;
}

/* 剥离行首 0+ 个 "name:" 标签（允许标签与指令同行，如 "L2: MOV R0, 1"）。
 * record=1 时把每个标签记录到 lname/laddr（地址=其后指令的地址），返回指向指令助记符的指针。 */
static char* asm_strip_labels(char* p, int addr, char lname[][32], int laddr[], int* nl, int record) {
    while (1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!pal_isalpha((unsigned char)*p)) break;
        char* q = p;
        while (pal_isalnum((unsigned char)*q) || *q == '_') q++;
        char* r = q;
        while (*r == ' ' || *r == '\t') r++;
        if (*r != ':') break;            /* 不是标签，p 指向指令助记符 */
        if (record) {
            int li = (int)(q - p);
            if (li > 0 && *nl < 256) {
                if (li > 31) li = 31;
                pal_memcpy(lname[*nl], p, li); lname[*nl][li] = 0; laddr[*nl] = addr; (*nl)++;
            }
        }
        p = r + 1;                       /* 跳过分号，继续看下一个标签/指令 */
    }
    return p;
}

int assemble(const char* asm_text, uint8_t* out, int cap, int* out_len, int* out_max_frame) {
    int nlines = 0;
    char** lines = split_lines(asm_text, &nlines);
    if (!lines) return -6;   /* 汇编文本拆分失败（内存不足），资源已由 split_lines 回滚 */

    char lname[256][32]; int laddr[256]; int nl = 0;

    /* 第一遍：计算地址 + 记录标签 */
    int addr = 0;
    for (int i = 0; i < nlines; i++) {
        char* ln = strip_comment(lines[i]);
        if (!ln) { for (int k = 0; k < nlines; k++) comp_free(lines[k]); comp_free(lines); return -6; }
        trim(ln);
        if (!*ln) { comp_free(ln); continue; }
        char mnem[32]; int ci = 0; char* p = ln;
        p = asm_strip_labels(p, addr, lname, laddr, &nl, 1);
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) { comp_free(ln); continue; }   /* 纯标签行 */
        ci = 0;
        while (*p && (pal_isalnum((unsigned char)*p) || *p == '_')) mnem[ci++] = *p++;
        mnem[ci] = 0;
        const op_t* op = find_op(mnem);
        if (!op) { comp_free(ln); for (int k = 0; k < nlines; k++) comp_free(lines[k]); comp_free(lines); return -1; }
        addr += 1;
        for (const char* f = op->fmt; *f; f++) {
            if (*f == 'R' || *f == 'T' || *f == 'B') addr += 1;
            else if (*f == 'H') addr += 2;
            else if (*f == 'I') addr += 4;
        }
        comp_free(ln);
    }

    /* 第二遍：发射字节 */
    int pos = 0;
    int maxf = 0;       /* 全局最大调用栈帧字节数（含 ret+fp 头） */
    for (int i = 0; i < nlines; i++) {
        char* ln = strip_comment(lines[i]);
        if (!ln) { for (int k = 0; k < nlines; k++) comp_free(lines[k]); comp_free(lines); return -6; }
        trim(ln);
        if (!*ln) { comp_free(ln); continue; }
        char mnem[32]; int ci = 0; char* p = ln;
        p = asm_strip_labels(p, 0, lname, laddr, &nl, 0);
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) { comp_free(ln); continue; }
        ci = 0;
        while (*p && (pal_isalnum((unsigned char)*p) || *p == '_')) mnem[ci++] = *p++;
        mnem[ci] = 0;
        const op_t* op = find_op(mnem);

        while (*p == ' ' || *p == '\t') p++;
        char ops[8][32]; int no = 0;
        while (*p && no < 8) {
            int oi = 0;
            while (*p && *p != ',' && *p != ' ' && *p != '\t') ops[no][oi++] = *p++;
            ops[no][oi] = 0; no++;
            if (*p == ',') p++;
            while (*p == ' ' || *p == '\t') p++;
        }

        if (pos + 1 > cap) { comp_free(ln); for (int k = 0; k < nlines; k++) comp_free(lines[k]); comp_free(lines); return -2; }
        out[pos++] = (uint8_t)op->op;

        int oi = 0;
        for (const char* f = op->fmt; *f; f++) {
            if (oi >= no) { comp_free(ln); for (int k = 0; k < nlines; k++) comp_free(lines[k]); comp_free(lines); return -3; }
            char* tok = ops[oi++];
            if (*f == 'R' || *f == 'T') {
                int rv = parse_reg(tok);
                if (rv < 0) { comp_free(ln); for (int k = 0; k < nlines; k++) comp_free(lines[k]); comp_free(lines); return -4; }
                out[pos++] = (uint8_t)rv;
            } else if (*f == 'B') {
                int v = parse_imm(tok); out[pos++] = (uint8_t)(v & 0xFF);
            } else if (*f == 'H') {
                int v = parse_imm(tok);
                out[pos++] = (uint8_t)(v & 0xFF); out[pos++] = (uint8_t)((v >> 8) & 0xFF);
            } else if (*f == 'I') {
                int v;
                if (is_label_tok(tok)) {
                    int found = -1;
                    for (int k = 0; k < nl; k++) if (!pal_strcmp(lname[k], tok)) { found = laddr[k]; break; }
                    if (found < 0) { comp_free(ln); for (int k = 0; k < nlines; k++) comp_free(lines[k]); comp_free(lines); return -5; }
                    v = found;
                } else {
                    v = parse_imm(tok);
                }
                uint32_t u = (uint32_t)v;
                out[pos++] = (uint8_t)(u & 0xFF);
                out[pos++] = (uint8_t)((u >> 8) & 0xFF);
                out[pos++] = (uint8_t)((u >> 16) & 0xFF);
                out[pos++] = (uint8_t)((u >> 24) & 0xFF);
            }
        }
        /* 记录全局最大调用栈帧（含 ret4 + fp4 + 局部 frame_sz = 8 + imm） */
        if (op->op == 41 && no >= 2) {       /* 汇编形态：CALL Rb, frame_sz */
            int fsz = parse_imm(ops[1]);
            int frame = 8 + fsz;
            if (frame > maxf) maxf = frame;
        }
        comp_free(ln);
    }

    for (int k = 0; k < nlines; k++) comp_free(lines[k]);
    comp_free(lines);
    *out_len = pos;
    if (out_max_frame) *out_max_frame = maxf;
    return 0;
}

/*
 * @brief  把 assemble 的返回码翻译成可读消息。
 * @param  int rc  assemble 的返回值（0 或负错误码）
 * @return const char*  静态消息串（不可修改、无需释放）
 * @note   目的是让调用方能给出具体诊断（如"标签未定义"），
 *         而非笼统的 "assembly failed"——后者无法指导用户排错。 */
const char* asm_err_msg(int rc)
{
    switch (rc) {
        case  0: return "OK";
        case -1: return "未知助记符";
        case -2: return "输出缓冲不足";
        case -3: return "操作数不足";
        case -4: return "寄存器解析失败";
        case -5: return "标签未定义";
        case -6: return "汇编文本拆分失败（内存不足）";
        default: return "未知汇编错误";
    }
}
