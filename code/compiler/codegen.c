/******************************************************************************
 * 代码生成：AST -> VM 文本汇编。
 *
 * 内存模型（与 VM 运行时一致）：
 *   - 全局标量 int/float  -> 4B 池(VM_POOL4)  LDRX/STRX
 *   - 全局 char          -> 1B 池(VM_POOL1)  LDRXB/STRXB
 *   - 全局 short         -> 2B 池(VM_POOL2)  LDRXH/STRXH
 *   - 全局数组/字符串    -> 句柄存于 4B 池；运行时 ALLOC 缓冲
 *   - 局部/参数          -> 函数帧(VM_FP 句柄)，4B 槽，LDR/STR [FP+off]
 *   - 数组/串元素动态下标 -> LDRH/STRH [句柄+idx*4]
 * 入口统一：PC=0 -> 全局前导 -> CALL main -> HALT；所有函数以 RET 收尾，
 * CALL 目标用 assembler 标签(func_xxx)回填，无需手填绝对地址。
 ******************************************************************************/
#include "codegen.h"
#include "error.h"
#include <stdarg.h>

/* ===================== 符号表 ===================== */
typedef struct {
    uint32_t name_off;    /* 字符串池偏移（原来是 char name[32]） */
    vtype_t type;       /* 标量/元素类型；str 时 T_STR */
    int    is_global;
    int    pool;        /* global 标量池 1/2/4；handle 全局用 4 */
    int    off;         /* global: 池内字节偏移；local: 帧内字节偏移(自 8) */
    int    is_param;
    int    is_handle;   /* 数组/字符串：槽内存的是堆句柄 */
    int    arr;         /* 数组元素数(>0)；str 为 0 */
    int    is_struct;   /* 结构体变量 */
    uint32_t sname_off;   /* 字符串池偏移（原来是 char struct_name[32]） */
    int    cached_reg;  /* 跨语句寄存器保持：局部标量当前常驻的物理寄存器，或 -1 表示在内存 */
} sym_t;

static sym_t* GSYM = NULL; static int GSYM_CAP = 0; static int NGSYM = 0;
/* 阶段 3B：符号表改为动态（从小容量起步，经 comp_realloc 翻倍）。
 * 注意：lookup() 返回的 sym_t* 不能跨 insert 持有，因为扩容可能搬动数组。
 * insert 只发生在 assign_decls() 与 gen_function() 的序言里 —— 严格早于
 * gen_stmt() 执行 —— 所以语句内部取到的指针在整个语句生命周期内都有效。 */
static sym_t* LSYM = NULL; static int LSYM_CAP = 0; static int NLSM = 0;
#define SYM_INIT_CAP 256

static int lsym_reserve(int need)
{
    if (need <= LSYM_CAP) return 1;
    int ncap = LSYM_CAP ? LSYM_CAP * 2 : SYM_INIT_CAP;
    while (ncap < need) ncap *= 2;
    sym_t* np = (sym_t*)comp_realloc(LSYM, (uint32_t)(LSYM_CAP * (int)sizeof(sym_t)), (uint32_t)(ncap * (int)sizeof(sym_t)));
    if (!np) return 0;
    LSYM = np; LSYM_CAP = ncap;
    return 1;
}

static int gsym_reserve(int need)
{
    if (need <= GSYM_CAP) return 1;
    int ncap = GSYM_CAP ? GSYM_CAP * 2 : SYM_INIT_CAP;
    while (ncap < need) ncap *= 2;
    sym_t* np = (sym_t*)comp_realloc(GSYM, (uint32_t)(GSYM_CAP * (int)sizeof(sym_t)), (uint32_t)(ncap * (int)sizeof(sym_t)));
    if (!np) return 0;
    GSYM = np; GSYM_CAP = ncap;
    return 1;
}
static int   LBASE;     /* 当前函数局部区起始帧偏移 = 8 + nparam*4 */

/* ===================== 函数元信息 ===================== */
typedef struct {
    char    name[32];
    int     nparam;
    vtype_t ptype[8];
    int     frame_imm;  /* CALL 第二操作数 = nparam*4 + 局部总字节 */
    vtype_t ret;        /* 推断返回类型 */
} fmeta_t;
static fmeta_t FMETA[64]; static int NFMETA = 0;
static vtype_t cur_ret = T_INT;

/* ===================== 输出缓冲 ===================== */
static char* G_OUT; static int G_CAP; static int G_OFF; static int G_LAB;

/*
 * @brief  格式化输出一段汇编文本到全局输出缓冲。
 * @param  const char* fmt  格式串（语义同 printf）
 * @param  ...              可变参数
 * @return int              实际写入的字符数；缓冲已满时返回 0
 * @note   ?安全要点：pal_vsnprintf 返回的是"本应写入"的字符数，可能大于剩余
 *         空间。必须把它钳制到真实剩余量再累加 G_OFF，否则 G_OFF 越过 G_CAP 后，
 *         下一次传入的长度参数为负、转 uint32_t 变成巨值，造成缓冲区溢出。
 *         缓冲满时采取"安全降级"（丢弃后续输出），由调用方比对 codegen() 的
 *         返回值与 cap 来发现截断。 */
static int out(const char* fmt, ...) {
    /* 缓冲已满：丢弃后续输出（安全降级，绝不越界） */
    if (G_OFF >= G_CAP) return 0;
    va_list ap; va_start(ap, fmt);
    int k = pal_vsnprintf(G_OUT + G_OFF, (uint32_t)(G_CAP - G_OFF), fmt, ap);
    va_end(ap);
    if (k < 0) k = 0;
    /* 关键：vsnprintf 返回的是「本应写入」的字符数，可能大于剩余空间。
     * 若直接 G_OFF += k，G_OFF 会越过 G_CAP，下一次 (G_CAP - G_OFF) 变成负数，
     * 转 uint32_t 后是巨值，pal_vsnprintf 会照此长度写入 —— 缓冲区溢出。
     * 故必须把 k 钳制到真实剩余空间。汇编输出因此可能被截断，
     * 由调用方用 codegen() 的返回值与 cap 比对来发现。 */
    int room = G_CAP - G_OFF;
    if (k > room) k = room;
    G_OFF += k;
    return k;
}
/*
 * @brief  分配一个新的标签编号（用于跳转目标）。
 * @param  无（无参数）
 * @return int  新的标签号（自增，首次为 0）
 * @note   生成的标签形如 L<n>；此处只负责编号，实际地址由 assembler
 *         在两遍扫描中回填，因此 codegen 无需知道绝对地址。 */
static int newlab(void) { return G_LAB++; }

/* ===================== 寄存器分配（仅 R6..R18，13 个通用临时） ===================== */
/* 调用约定：R1..R5=参数，R0=返回值，R6=函数地址临时；
 * R6..R18 为通用临时寄存器，被调用者不保证保留，故 CALL 前需 caller-save 溢出。
 *
 * R19..R29 是 VM 保留特殊寄存器，绝不能当通用临时：
 *   R19=VM_DATA, R20=VM_POOL1, R21=VM_POOL2, R22=VM_POOL4（裸地址基址），
 *   R23=VM_ZR（零寄存器，vm_step 在每条指令后强制置 0），
 *   R24=VM_CMP_FLAG，R25..R29=中断/溢出/进位/负/零 标志位。
 * 旧实现把 R6..R29 全当临时，会把下标/循环计数器"+1"分配进零寄存器 R23，
 * 被 vm_step 每步清零，导致 i = i + 0 死循环（问题 5：嵌套循环+数组读触顶步数上限）。
 * 故只分配 R6..R18（13 个），R19..R29 一律保留给 VM。
 *
 * 限制：没有寄存器溢出(spill)机制。若同时存活的临时量超过 13 个，
 * reg_alloc 直接报编译错误（而非静默复用 R6 产生错误结果）。 */
static int REG[13]; static int RSP = 13;
static int  g_reg_ovf_reported = 0;
static uint32_t g_cached_mask = 0;   /* 位图：哪些通用寄存器当前被「局部变量常驻」占用 */
static uint32_t g_dirty_mask = 0;    /* 位图：常驻变量中哪些被写过（内存可能滞后），冲刷时只需写回脏的 */
/* 最多同时常驻的局部变量数：留出余量给表达式临时寄存器，避免临时寄存器耗尽。
 * 超出的变量退化为「普通 load/store」（内存为唯一真相源），不产生正确性风险。 */
#define MAX_PROMOTE 8
static int n_promoted(void) { int n = 0; for (int r = 6; r <= 18; r++) if (g_cached_mask & (1u << r)) n++; return n; }

/* 寄存器是否正被某局部变量常驻占用（跨语句保持） */
static int reg_is_cached(int r) { return (r >= 6 && r <= 18) ? (int)((g_cached_mask >> r) & 1u) : 0; }

/* 把某个常驻寄存器还给空闲池（变量被踢出/冲刷时调用） */
static void reg_drop_cached(int r) {
    g_cached_mask &= ~(1u << r);
    g_dirty_mask &= ~(1u << r);
    if (RSP < 13) REG[RSP++] = r;
}

/*
 * @brief  初始化寄存器池（每个函数开始生成前调用）。
 * @param  无（无参数）
 * @return 无（void）
 * @note   把 R6..R18 共 13 个通用寄存器压入空闲栈 REG，并清空常驻/脏位图
 *         与"溢出已报告"标志。R19..R29 一律保留给 VM，绝不参与分配
 *         （详见文件头的寄存器约定）。 */
static void reg_init(void) { for (int i = 0; i < 13; i++) REG[i] = 6 + i; RSP = 13; g_reg_ovf_reported = 0; g_cached_mask = 0; g_dirty_mask = 0; }

/* 踢出一个常驻变量：仅当被写过才写回帧槽，释放其寄存器（寄存器不足时溢出用） */
/*
 * @brief  踢出一个"常驻局部变量"，释放它占用的寄存器。
 * @param  无（无参数）
 * @return int  被释放的寄存器号（R6..R18）；没有可踢出的变量时返回 -1
 * @note   仅当该变量被写过（dirty 位置位）才写回帧槽；未写过则直接丢弃，
 *         因为内存中的值仍然有效。用于空闲寄存器耗尽时的溢出处理。 */
static int evict_one(void) {
    for (int i = 0; i < NLSM; i++) {
        if (LSYM[i].cached_reg >= 0) {
            int r = LSYM[i].cached_reg;
            if (g_dirty_mask & (1u << r)) out("STR R%d, R%d, %d\n", r, VM_FP, LSYM[i].off);
            LSYM[i].cached_reg = -1;
            reg_drop_cached(r);
            return r;
        }
    }
    return -1;
}

/*
 * @brief  分配一个通用临时寄存器。
 * @param  无（无参数）
 * @return int  寄存器号（R6..R18）
 * @note   优先从空闲栈取；栈空则踢出一个常驻变量（evict_one）；两者都失败
 *         说明同时存活的临时量超过 13 个——此时**报编译错误**而非静默复用
 *         寄存器（静默复用会产出错误结果）。仍兜底返回 R6 以便继续报出更多
 *         错误；产物不可信，最终会因错误计数非零而中止编译。 */
static int reg_alloc(void) {
    if (RSP > 0) return REG[--RSP];
    /* 无空闲寄存器：踢出一个常驻变量腾出位置（其内存已被写回，后续按需重载） */
    int ev = evict_one();
    if (ev >= 0) return ev;
    if (!g_reg_ovf_reported) {
        g_reg_ovf_reported = 1;
        comp_error(0, COMP_ERR_INTERNAL,
            "寄存器不足：同时存活的临时变量超过 13 个(R6..R18)，需要 spill 但尚未实现。"
            "请减少同一作用域内同时存在的局部变量/复杂表达式，或拆分大表达式。");
    }
    return 6;   /* 兜底：继续编译以便报出更多错误，但产物不可信，编译会因错误计数非零而中止 */
}
/* free：若 r 正被某变量常驻占用则保留；否则放回空闲池。
 * 关键：防止同一寄存器被重复放回（例如 E_BIN 的左右子表达式求值返回同一 home 寄存器时，
 * reg_free(rl) 与 reg_free(rr) 是同一个 r，会被 free 两次），否则 REG 栈出现重复值，
 * 后续 reg_alloc 会对两个临时量返回同一物理寄存器，进而与某常驻变量的 home 冲突、破坏数据。
 * 重复放回检查保证「REG 栈中的寄存器互不重复且与 g_cached_mask 一致」这一不变式。 */
/*
 * @brief  归还一个临时寄存器到空闲池。
 * @param  int r  待归还的寄存器号
 * @return 无（void）
 * @note   三种情况不归还：①不在 R6..R18 范围 ②正被某变量常驻占用
 *         ③已在空闲栈中（防止重复放回）。
 *         ?重复放回检查至关重要：E_BIN 的左右子表达式求值可能返回同一个
 *         home 寄存器，若被 free 两次会让 REG 栈出现重复值，后续 reg_alloc
 *         会对两个临时量返回同一物理寄存器，进而破坏数据。此检查维护了
 *         "REG 栈中寄存器互不重复且与 g_cached_mask 一致"这一不变式。 */
static void reg_free(int r) {
    if (r < 6 || r > 18 || reg_is_cached(r) || RSP >= 13) return;
    for (int i = 0; i < RSP; i++) if (REG[i] == r) return;  /* 已在空闲栈，跳过重复放回 */
    REG[RSP++] = r;
}

/* 若 r 是某局部变量的常驻 home 寄存器，把它的值复制到一个独立的临时寄存器。
 *
 * 为什么必须这么做（E_BIN 的左操作数）：
 *   lv_get() 对局部变量返回的是常驻 home，而不是新分配的临时寄存器。
 *   若随后 gen_expr(右操作数) 里出现 CALL，lv_flush_all() 会把这个 home
 *   释放回 REG 空闲池；E_CALL 末尾的 reg_alloc() 又可能把它分配给返回值，
 *   于是左操作数被覆盖。
 *   实测：fact(k)=k*fact(k-1) 会算出 1（两个操作数都变成了返回值）。
 *   换成独立临时寄存器后，它不属于任何 LSYM，不会被 flush/evict 释放，
 *   也不会被 reg_alloc 再次分配（因为它一直没被 reg_free）。 */
static int detach_if_cached(int r) {
    if (reg_is_cached(r)) {
        int t = reg_alloc();
        out("MOV R%d, R%d\n", t, r);
        return t;
    }
    return r;
}

/* ===================== 局部标量「跨语句寄存器保持」辅助 ===================== */
/* 取得持有变量 s 值的寄存器（s 的固定 home 寄存器）。
 * 关键：home 寄存器在 s 的整个生存期内固定不变，已发射的代码（如循环条件）
 * 引用该 home 后绝不会因后续赋值而失效。首次使用时从帧载入并常驻；
 * 若已达提升上限则退化为普通 load（不常驻），内存始终是唯一真相源。 */
static int lv_get(sym_t* s) {
    if (s->cached_reg >= 0) return s->cached_reg;
    if (n_promoted() >= MAX_PROMOTE) { int r = reg_alloc(); out("LDR R%d, R%d, %d\n", r, VM_FP, s->off); return r; }
    int r = reg_alloc();
    out("LDR R%d, R%d, %d\n", r, VM_FP, s->off);
    s->cached_reg = r; g_cached_mask |= (1u << r);
    return r;
}
/* 把变量 s 的值绑定到寄存器 r：结果搬入 s 的固定 home 寄存器（不变更 home）。
 *   - s 尚未常驻：分配固定 home h；若已达上限则退化为普通 STR 到内存。
 *   - r == h：结果已在 home（如复合赋值原地计算），不必搬运。
 *   - r != h：MOV h, r；若 r 是临时结果则释放，若 r 恰是别的变量 home 则保留（复制语义）。 */
static void lv_bind(sym_t* s, int r) {
    int h = s->cached_reg;
    if (h < 0) {
        if (n_promoted() >= MAX_PROMOTE) {
            out("STR R%d, R%d, %d\n", r, VM_FP, s->off);   /* 退化：直接写回内存，不常驻 */
            if (!reg_is_cached(r)) reg_free(r);
            return;
        }
        h = reg_alloc();
        s->cached_reg = h;
        g_cached_mask |= (1u << h);
    }
    if (r != h) {
        out("MOV R%d, R%d\n", h, r);
        if (!reg_is_cached(r)) reg_free(r);   /* r 为临时值则释放；为别的变量 home 则保留 */
    }
    /* lazy store：只标记脏，不立即写回内存。本语言局部标量无取地址、无引用传参，
     * 且 home 寄存器在生存期固定不变（不会因赋值而失效），因此缓存值始终是最新的；
     * 内存仅在「同步点」冲刷：函数调用前(lv_flush_all，因 CALL 会破坏 R6..R18)、
     * 函数返回前(emit_ret)、寄存器溢出(evict_one)。
     * 这样循环体内赋值不再发 STR，回边 lv_get 命中缓存也不发 LDR，访存流量大幅下降。
     * 退化路径(超 MAX_PROMOTE)仍走内存真相源，见上方 str。 */
    g_dirty_mask |= (1u << h);
}
/* 把所有「脏」的常驻变量冲刷回内存并释放寄存器（CALL / RET 等同步点调用） */
static void lv_flush_all(void) {
    for (int i = 0; i < NLSM; i++) {
        if (LSYM[i].cached_reg >= 0) {
            int r = LSYM[i].cached_reg;
            if (g_dirty_mask & (1u << r)) out("STR R%d, R%d, %d\n", r, VM_FP, LSYM[i].off);
            LSYM[i].cached_reg = -1;
            reg_drop_cached(r);
        }
    }
}
static void emit_ret(void) { lv_flush_all(); out("RET\n"); }

/* 循环前预取：把条件表达式引用的所有局部标量加载进缓存，使循环头处的 lv_get 命中，
 * 从而消除循环回边的冗余 LDR。配合 lazy store（赋值不立即写回内存），缓存即最新值，
 * 回边不再从内存重载，循环体访存趋于零。
 * 全局/句柄/数组不缓存（每次都从内存读），无需预取。函数调用参数若被预取，调用时的
 * lv_flush_all 会清空缓存，但内存已是最新，回边重新 LDR 仍正确（仅失去优化，无正确性风险）。 */
static sym_t* lookup(const char* nm);
static void lv_prefetch_cond(expr_t* e) {
    if (!e) return;
    switch (e->kind) {
        case E_VAR: { sym_t* s = lookup(sp_get(e->name_off));
            if (s && !s->is_global && !s->is_handle && !s->is_struct) (void)lv_get(s); break; }
        case E_BIN: lv_prefetch_cond(e->l); lv_prefetch_cond(e->r); break;
        case E_UN:  lv_prefetch_cond(e->l); break;
        case E_CAST: lv_prefetch_cond(e->l); break;
        case E_CALL: { expr_t* a = e->l; while (a) { lv_prefetch_cond(a); a = a->next; } break; }
        default: break;
    }
}

/* 递归预取语句树中所有局部标量变量。在进入循环(Lstart)前对整棵循环语句树调用，
 * 使循环内（含嵌套循环/if/调用/输入/声明）每个局部标量的「首次 lv_get」在循环前完成
 * 并常驻缓存；循环中及回边的 lv_get 全部命中缓存、不再从内存重载。配合 lazy store，
 * 既保证正确性（缓存即最新值）又让循环体访存趋于零。
 * 退化变量(超 MAX_PROMOTE)由 lv_bind 每次 STR 回内存，回边 LDR 读到的也是最新值，同样正确。 */
static void lv_touch_stmt(stmt_t* s) {
    while (s) {
        switch (s->kind) {
            case S_DECL:
                if (s->init) lv_prefetch_cond(s->init);
                { sym_t* sy = lookup(sp_get(s->name_off));
                  if (sy && !sy->is_global && !sy->is_handle && !sy->is_struct) (void)lv_get(sy); }
                break;
            case S_ASSIGN:
                if (s->lhs_expr) lv_prefetch_cond(s->lhs_expr);
                if (s->lhs_idx)  lv_prefetch_cond(s->lhs_idx);
                if (s->e)        lv_prefetch_cond(s->e);
                break;
            case S_PRINT:  if (s->e) lv_prefetch_cond(s->e); break;
            case S_INPUT: {
                sym_t* sy = lookup(sp_get(s->name_off));
                if (sy && !sy->is_global && !sy->is_handle && !sy->is_struct) (void)lv_get(sy);
                break;
            }
            case S_RETURN: if (s->e) lv_prefetch_cond(s->e); break;
            case S_EXPR:   if (s->e) lv_prefetch_cond(s->e); break;
            case S_IF:
                if (s->cond) lv_prefetch_cond(s->cond);
                lv_touch_stmt(s->body); lv_touch_stmt(s->els);
                break;
            case S_WHILE:
                if (s->cond) lv_prefetch_cond(s->cond);
                lv_touch_stmt(s->body);
                break;
            case S_DOWHILE:
                lv_touch_stmt(s->body);
                if (s->cond) lv_prefetch_cond(s->cond);
                break;
            case S_FOR:
                if (s->init_s) lv_touch_stmt(s->init_s);
                if (s->cond)   lv_prefetch_cond(s->cond);
                if (s->step_s) lv_touch_stmt(s->step_s);
                lv_touch_stmt(s->body);
                break;
            case S_BLOCK:
                lv_touch_stmt(s->body);
                break;
        }
        s = s->next;
    }
}

/* 返回当前已分配(存活)的物理寄存器列表，用于 CALL 前溢出 */
static int reg_live_list(int* out) {
    int freebit[32]; for (int i = 0; i < 32; i++) freebit[i] = 0;
    for (int i = 0; i < RSP; i++) freebit[REG[i]] = 1;
    int cnt = 0;
    for (int r = 6; r <= 18; r++) if (!freebit[r]) out[cnt++] = r;
    return cnt;
}

static uint32_t fbits(float f) { union { float f; uint32_t u; } x; x.f = f; return x.u; }

/* ===================== 符号查找 ===================== */
/*
 * @brief  按名字查找符号（先查局部，再查全局）。
 * @param  const char* nm  变量名
 * @return sym_t*          找到的符号表项；未找到返回 NULL
 * @note   局部优先（LSYM 遮蔽 GSYM），符合块作用域语义。
 *         由于 sym_t.name 是内嵌 char[32]（未池化），此处可直接 pal_strcmp
 *         比对，无需 sp_get——这是符号表刻意保留内嵌数组的原因。 */
static sym_t* lookup(const char* nm) {
    for (int i = 0; i < NLSM; i++) if (!pal_strcmp(sp_get(LSYM[i].name_off), nm)) return &LSYM[i];
    for (int i = 0; i < NGSYM; i++) if (!pal_strcmp(sp_get(GSYM[i].name_off), nm)) return &GSYM[i];
    return NULL;
}

/* ===================== 结构体支持 ===================== */
static void emit_load(sym_t* s, int rd);
static void emit_store(sym_t* s, int rs);
static struct_def_t* G_SDEFS[64]; static int G_NSDEF = 0;
/*
 * @brief  按名字查找结构体定义。
 * @param  const char* nm  结构体类型名
 * @return struct_def_t*   找到的定义；未找到返回 NULL
 * @note   遍历 prog_t->structs（最多 64 个）。用于解析结构体变量声明与
 *         嵌套字段访问时获取字段布局。 */
static struct_def_t* find_struct(const char* nm) {
    for (int i = 0; i < G_NSDEF; i++) if (!pal_strcmp(sp_get(G_SDEFS[i]->name_off), nm)) return G_SDEFS[i];
    return NULL;
}
/*
 * @brief  在结构体定义中按名字查找字段，返回其偏移/类型/嵌套类型名。
 * @param  struct_def_t* sd  结构体定义
 * @param  const char* fn    字段名
 * @param  int* off          输出：字段在结构体内的字节偏移
 * @param  vtype_t* ft       输出：字段类型
 * @param  char* fsname      输出：若字段是嵌套结构体，写入其类型名（否则写空串）
 * @return int               1=找到，0=未找到
 * @note   线性扫描 nfield 个字段（≤32，代价可忽略）。当前实现按"每个字段
 *         固定 4 字节"计算偏移（i*4），故不支持 1/2 字节字段的紧凑布局。 */
static int find_field(struct_def_t* sd, const char* fn, int* off, vtype_t* ft, char* fsname) {
    if (!sd) return 0;
    for (int i = 0; i < sd->nfield; i++) {
        if (!pal_strcmp(sp_get(sd->fname_off[i]), fn)) {
            *off = i * 4; *ft = sd->ftype[i]; pal_strncpy(fsname, sp_get(sd->fsname_off[i]), 32);
            return 1;
        }
    }
    return 0;
}
static int struct_size(const char* nm) {
    struct_def_t* sd = find_struct(nm);
    return sd ? sd->nfield * 4 : 4;
}
/* 返回字段最终类型（不分配寄存器，供 infer 使用） */
static vtype_t dot_resolve_type(expr_t* e, char* out_sname) {
    out_sname[0] = 0;
    if (e->kind == E_VAR) {
        sym_t* s = lookup(sp_get(e->name_off));
        if (!s || !s->is_struct) return T_INT;
        pal_strncpy(out_sname, sp_get(s->sname_off), 32); return T_INT;
    }
    if (e->kind == E_DOT) {
        char csname[32]; vtype_t ct = dot_resolve_type(e->l, csname); (void)ct;
        struct_def_t* sd = find_struct(csname);
        if (!sd) return T_INT;
        int foff; vtype_t ft; char fsname[32];
        if (!find_field(sd, sp_get(e->field_off), &foff, &ft, fsname)) return T_INT;
        if (ft == T_STRUCT) pal_strncpy(out_sname, fsname, 32);
        return ft;
    }
    return T_INT;
}
/* 解析结构体字段地址：返回 base 寄存器（VM_FP 或新分配句柄寄存器，调用方在 !=VM_FP 时释放），
 * off 为字段相对 base 的字节偏移，leaf_ftype 为最终字段类型。 */
/* 前向声明：resolve_dot 需要调用 gen_expr 计算数组下标表达式，
 * 而 gen_expr 的实际定义在文件更后面（见其前向声明处）。 */
static int gen_expr(expr_t* e);

static int resolve_dot(expr_t* e, int* off, char* container_sname, vtype_t* leaf_ftype) {
    if (e->kind == E_VAR) {
        sym_t* s = lookup(sp_get(e->name_off));
        if (!s || !s->is_struct) { *off = 0; *leaf_ftype = T_INT; container_sname[0] = 0; return VM_FP; }
        if (!s->is_global) { *off = s->off; pal_strncpy(container_sname, sp_get(s->sname_off), 32); *leaf_ftype = T_INT; return VM_FP; }
        int rh = reg_alloc(); emit_load(s, rh); *off = 0; pal_strncpy(container_sname, sp_get(s->sname_off), 32); *leaf_ftype = T_INT; return rh;
    }
    if (e->kind == E_DOT) {
        int reg = resolve_dot(e->l, off, container_sname, leaf_ftype);
        struct_def_t* sd = find_struct(container_sname);
        if (!sd) { *leaf_ftype = T_INT; return reg; }
        int foff; vtype_t ft; char fsname[32];
        if (!find_field(sd, sp_get(e->field_off), &foff, &ft, fsname)) { *leaf_ftype = T_INT; return reg; }
        *off += foff;
        if (ft == T_STRUCT) pal_strncpy(container_sname, fsname, 32);
        *leaf_ftype = ft;
        return reg;
    }
    *off = 0; *leaf_ftype = T_INT; container_sname[0] = 0; return VM_FP;
}

/* ===================== 类型推断 ===================== */
/*
 * @brief  推断表达式的结果类型（int / float / str 等）。
 * @param  expr_t* e  表达式节点
 * @return vtype_t    推断出的类型
 * @note   递归推断：E_BIN 时只要有一侧是 T_FLOAT 就提升为 T_FLOAT；
 *         E_VAR/E_CALL 依赖符号表或函数元信息；比较与逻辑运算结果为 T_INT。
 *         用于决定生成整数指令还是浮点指令。 */
static vtype_t infer(expr_t* e) {
    if (!e) return T_INT;
    switch (e->kind) {
        case E_NUM: case E_CNUM: return T_INT;
        case E_FNUM: return T_FLOAT;
        case E_STR:  return T_STR;
        case E_VAR: {
            sym_t* s = lookup(sp_get(e->name_off));
            if (!s) return T_INT;
            if (s->is_handle) return (s->type == T_STR) ? T_STR : T_INT;
            return s->type;
        }
        case E_IDX: { sym_t* s = lookup(sp_get(e->l->name_off)); return s ? s->type : T_INT; }
        case E_CALL:
            for (int i = 0; i < NFMETA; i++) if (!pal_strcmp(FMETA[i].name, sp_get(e->name_off))) return FMETA[i].ret;
            return T_INT;
        case E_CAST: return e->ctype;
        case E_UN: return (e->uop == 1) ? T_INT : infer(e->l);
        case E_BIN:
            if (e->op >= OP_LT && e->op <= OP_OR) return T_INT;  /* 比较/逻辑 -> 布尔 */
            { vtype_t a = infer(e->l), b = infer(e->r);
              if (a == T_FLOAT || b == T_FLOAT) return T_FLOAT; return T_INT; }
        case E_DOT: { char sname[32]; return dot_resolve_type(e, sname); }
    }
    return T_INT;
}

/* ===================== 符号加载/存储 ===================== */
static void emit_load(sym_t* s, int rd) {
    if (s->is_global) {
        if (s->is_handle || s->type == T_INT || s->type == T_FLOAT)
            out("LDRX R%d, R%d, %d\n", rd, VM_POOL4, s->off);
        else if (s->type == T_CHAR)
            out("LDRXB R%d, R%d, %d\n", rd, VM_POOL1, s->off);
        else if (s->type == T_SHORT)
            out("LDRXH R%d, R%d, %d\n", rd, VM_POOL2, s->off);
    } else {
        out("LDR R%d, R%d, %d\n", rd, VM_FP, s->off);
    }
}
static void emit_store(sym_t* s, int rs) {
    if (s->is_global) {
        if (s->is_handle || s->type == T_INT || s->type == T_FLOAT)
            out("STRX R%d, R%d, %d\n", rs, VM_POOL4, s->off);
        else if (s->type == T_CHAR)
            out("STRXB R%d, R%d, %d\n", rs, VM_POOL1, s->off);
        else if (s->type == T_SHORT)
            out("STRXH R%d, R%d, %d\n", rs, VM_POOL2, s->off);
    } else {
        out("STR R%d, R%d, %d\n", rs, VM_FP, s->off);
    }
}

/* ===================== 字符串缓冲(布局: [4B 长度][字符...]) ===================== */
static void gen_str_fill(int rh, const char* s, int len) {
    int rt = reg_alloc();
    int ro = reg_alloc();
    out("MOV32 R%d, %d\n", rt, len);
    /* 长度头在偏移 0：直接用硬连线零寄存器 VM_ZR 作偏移，
     * 省掉"借一个寄存器装常量 0"的 MOV32（ro 仍用于后续非 0 的逐字符偏移）。 */
    out("STRH R%d, R%d, R%d\n", rt, rh, VM_ZR);
    for (int i = 0; i < len; i++) {
        int rc = reg_alloc();
        out("MOV32 R%d, %d\n", rc, (unsigned char)s[i]);
        out("MOV32 R%d, %d\n", ro, 4 + i * 4);
        out("STRH R%d, R%d, R%d\n", rc, rh, ro);
        reg_free(rc);
    }
    reg_free(rt); reg_free(ro);
}
/* 分配新缓冲并填入字符串字面量，返回句柄寄存器 */
static int gen_str_literal(expr_t* e) {
    int r = reg_alloc();
    int len = e->ival;
    out("ALLOC R%d, 0, %d\n", r, len * 4 + 4);
    gen_str_fill(r, sp_get(e->name_off), len);
    return r;
}

/* ===================== 表达式生成（返回持有值的寄存器） ===================== */
static int gen_expr(expr_t* e);
static int gen_bool(expr_t* e);

/*
 * @brief  生成表达式的求值指令（AST 表达式 -> 汇编）。
 * @param  expr_t* e  表达式节点
 * @return int        存放结果的寄存器号
 * @note   ?采用后序遍历：先递归生成左、右子树的代码，再发射本节点的运算
 *         指令——这与栈式 VM 的"先操作数后运算"执行模型完全吻合，
 *         表达式的优先级/嵌套顺序在 AST 阶段已确定，此处只做机械翻译。
 *         各 kind 的分派：E_NUM/E_FNUM 取立即数、E_VAR 载入变量、
 *         E_BIN 二元运算、E_CALL 发射 CALL、E_IDX 数组元素、E_DOT 字段访问。 */
static int gen_expr(expr_t* e) {
    switch (e->kind) {
        case E_NUM: case E_CNUM: {
            int r = reg_alloc(); out("MOV32 R%d, %d\n", r, e->ival); return r;
        }
        case E_FNUM: {
            int r = reg_alloc(); out("MOV32 R%d, %uu\n", r, (unsigned)fbits(e->fval)); return r;
        }
        case E_STR:
            return gen_str_literal(e);
        case E_VAR: {
            sym_t* s = lookup(sp_get(e->name_off));
            /* 未声明的变量：必须报错。此前这里静默生成"常量 0"，
             * 用户拼错变量名时会得到能编译通过、但结果错误且无任何提示的产物，
             * 属于最难排查的一类问题。报错后仍兜底生成 0 以便继续报出更多错误，
             * 产物最终因错误计数非零而作废。 */
            if (!s) {
                comp_error(0, COMP_ERR_UNDECLARED, "未声明的变量 '%s'",
                           sp_get(e->name_off));
                int r = reg_alloc(); out("MOV32 R%d, 0\n", r); return r;
            }
            if (s->is_struct) { comp_error(0, COMP_ERR_TYPE, "struct value used as scalar"); int r = reg_alloc(); out("MOV32 R%d, 0\n", r); return r; }
            if (s->is_global || s->is_handle) { int r = reg_alloc(); emit_load(s, r); return r; }
            return lv_get(s);   /* 局部标量：跨语句常驻，复用已缓存寄存器 */
        }
        case E_IDX: {
            sym_t* s = lookup(sp_get(e->l->name_off));
            int rh = reg_alloc(); if (s) emit_load(s, rh);
            int ri = gen_expr(e->r);
            int rt = reg_alloc(); out("SHLI R%d, R%d, 2\n", rt, ri);
            int rd = reg_alloc(); out("LDRH R%d, R%d, R%d\n", rd, rh, rt);
            reg_free(rh); reg_free(ri); reg_free(rt);
            return rd;
        }
        case E_DOT: {
            int off; char csname[32]; vtype_t ft;
            int reg = resolve_dot(e, &off, csname, &ft);
            int rd = reg_alloc();
            out("LDR R%d, R%d, %d\n", rd, reg, off);
            if (reg != VM_FP) reg_free(reg);
            return rd;
        }
        case E_CALL: {
            int argc = 0; int areg[8];
            expr_t* a = e->l;
            while (a && argc < 8) { areg[argc++] = gen_expr(a); a = a->next; }
            for (int i = 0; i < argc; i++) { out("MOV R%d, R%d\n", i + 1, areg[i]); reg_free(areg[i]); }
            /* 调用前先把所有常驻局部变量冲刷回内存：被调函数不保留 R6..R18，
             * 且 callee 可能读写全局，必须保证内存视图最新；冲刷后缓存清空，调用后按需重载 */
            /* caller-save 的存活列表必须在 lv_flush_all 之前取：
             * lv_flush_all 里的 reg_drop_cached 会把常驻变量寄存器释放回
             * REG 空闲池，使 reg_live_list 把它们误判为"不存活"而不保存，
             * 被调函数随后就能复用这些寄存器，调用返回后左操作数已损坏。
             * 典型症状：fact(k) = k * fact(k-1) 算出 1 而非 120。 */
            int live[24], nl = reg_live_list(live);
            lv_flush_all();
            int spill_base = LBASE;   /* 帧内局部区(含结构体)之后的溢出区 */
            for (int i = 0; i < nl; i++)
                out("STR R%d, R%d, %d\n", live[i], VM_FP, spill_base + (live[i] - 6) * 4);
            fmeta_t* m = NULL;
            for (int i = 0; i < NFMETA; i++) if (!pal_strcmp(FMETA[i].name, sp_get(e->name_off))) m = &FMETA[i];
            /* 未找到函数定义：常见于把变量当函数调用（int x; ... x()）。
             * 此时 func_xxx 标签并不存在，assembler 只会给出含糊的"assembly failed"，
             * 故在此显式报错，让用户知道真正原因。仍继续生成代码以便报出更多错误，
             * 产物最终会因错误计数非零而作废。 */
            if (!m) {
                /* 用 UNDECLARED 而非 INTERNAL：这是"用户调用了不存在的函数"，
                 * 属于语义诊断，不是编译器内部故障。 */
                comp_error(0, COMP_ERR_UNDECLARED,
                           "调用了未定义的函数 '%s'（变量不能当函数调用）",
                           sp_get(e->name_off));
            } else if (argc != m->nparam || a != NULL) {
                /* 实参个数必须与形参一致。否则：
                 * - 少传：被调函数读取未写入的参数寄存器（实测形参 3 个只传 2 个时，
                 *   第 3 个参数取到 0，编译却完全通过）；
                 * - 多传：多余的实参被静默丢弃。
                 * a != NULL 表示实参超过 8 个（上面的 while 有 argc < 8 上限）而被截断，
                 * 此时即便 argc 恰好等于 nparam，实际实参也更多，同样要报错。 */
                comp_error(0, COMP_ERR_TYPE,
                           "函数 '%s' 调用时实参个数与形参个数(%d)不匹配",
                           sp_get(e->name_off), m->nparam);
            }
            /* 装函数地址的寄存器必须从池中借用，不能硬编码 R6：
             * 第 6 个实参的传递寄存器正是 R6，硬编码会先把实参 6 写入 R6、
             * 再用「MOV32 R6, func_xxx」把它覆盖成函数地址，导致被调函数收到
             * 的第 6 个参数是函数地址（实测 g(1..8) 时第 6 个参数变成 func_g
             * 的地址而非 6）。实参最多 8 个（占 R18..R11 且随后已释放），故此处
             * 借到的寄存器必在 R11 及以上，与参数寄存器 R1..R8 不冲突。 */
            int ra = reg_alloc();
            out("MOV32 R%d, func_%s\n", ra, sp_get(e->name_off));
            out("CALL R%d, %d\n", ra, m ? m->frame_imm : 0);
            reg_free(ra);
            for (int i = 0; i < nl; i++)
                out("LDR R%d, R%d, %d\n", live[i], VM_FP, spill_base + (live[i] - 6) * 4);
            int rres = reg_alloc(); out("MOV R%d, R0\n", rres);
            return rres;
        }
        case E_CAST: {
            int r = gen_expr(e->l);
            vtype_t vt = infer(e->l);
            /* 若会就地改写 r，且 r 是某变量的常驻寄存器，先复制到新临时，避免破坏变量 */
            int docopy = 0;
            if (e->ctype == T_FLOAT && vt != T_FLOAT) docopy = 1;
            else if (e->ctype == T_INT && vt == T_FLOAT) docopy = 1;
            else if (e->ctype == T_CHAR) docopy = 1;
            else if (e->ctype == T_SHORT) docopy = 1;
            if (docopy && reg_is_cached(r)) { int t = reg_alloc(); out("MOV R%d, R%d\n", t, r); r = t; }
            if (e->ctype == T_FLOAT) { if (vt != T_FLOAT) out("I2F R%d, R%d\n", r, r); }
            else if (e->ctype == T_INT) { if (vt == T_FLOAT) out("F2I R%d, R%d\n", r, r); }
            else if (e->ctype == T_CHAR) {
                if (vt == T_FLOAT) out("F2I R%d, R%d\n", r, r);
                int rt = reg_alloc(); out("MOV32 R%d, 255\n", rt); out("AND R%d, R%d, R%d\n", r, r, rt); reg_free(rt);
            } else if (e->ctype == T_SHORT) {
                if (vt == T_FLOAT) out("F2I R%d, R%d\n", r, r);
                int rt = reg_alloc(); out("MOV32 R%d, 65535\n", rt); out("AND R%d, R%d, R%d\n", r, r, rt); reg_free(rt);
            }
            return r;
        }
        case E_UN: {
            if (e->uop == 1) { /* 逻辑非：结果写入新临时，避免覆盖源（可能是常驻变量） */
                int r = gen_bool(e->l);
                /* XOR 在指令表中是 RRR 格式（见 assembler.c），第三操作数必须是寄存器，
                 * 不能直接写立即数 1，否则汇编阶段报"寄存器解析失败"。
                 * 这里先用临时寄存器装载 1，与下方取负的写法保持一致。 */
                int z = reg_alloc(); out("MOV32 R%d, 1\n", z);
                int rt = reg_alloc(); out("XOR R%d, R%d, R%d\n", rt, r, z);
                reg_free(z); reg_free(r); return rt;
            }
            if (e->uop == 2) { /* 后增：返回旧值，新值常驻回变量（不写回内存） */
                sym_t* s = lookup(sp_get(e->l->name_off));
                int rold = reg_alloc();
                if (s) { int rc = lv_get(s); out("MOV R%d, R%d\n", rold, rc); }
                int rnew = reg_alloc();
                if (s && s->type == T_FLOAT) { int z = reg_alloc(); out("MOV32 R%d, %uu\n", z, (unsigned)fbits(1.0f)); out("ADDF R%d, R%d, R%d\n", rnew, rold, z); reg_free(z); }
                else { int z = reg_alloc(); out("MOV32 R%d, 1\n", z); out("ADD R%d, R%d, R%d\n", rnew, rold, z); reg_free(z); }
                if (s) lv_bind(s, rnew);
                return rold;
            }
            /* 负号：结果写入新临时（先复制自源，避免覆盖常驻变量） */
            int r = gen_expr(e->l);
            if (reg_is_cached(r)) { int t = reg_alloc(); out("MOV R%d, R%d\n", t, r); r = t; }
            vtype_t vt = infer(e->l);
            /* 取负 = 0 - r：被减数恒为 0，直接用零寄存器 VM_ZR，
             * 省掉一次寄存器借用和一条 MOV32（原写法借 z 只为装常量 0）。 */
            if (vt == T_FLOAT) out("SUBF R%d, R%d, R%d\n", r, VM_ZR, r);
            else               out("SUB R%d, R%d, R%d\n", r, VM_ZR, r);
            return r;
        }
        case E_BIN: {
            /* "str" + "str" 以前是把两个「句柄」相加（print("ab"+"cd")
             * 打印出 3）。字符串拼接尚未实现，所以这里直接报错，
             * 而不是产出一个没有意义的数字。 */
            if (e->op == OP_ADD && infer(e->l) == T_STR && infer(e->r) == T_STR) {
                comp_error(0, COMP_ERR_SYNTAX,
                           "string concatenation is not supported; concatenate manually");
            }
            if (e->op >= OP_LT && e->op <= OP_NE) {  /* 比较 -> 物化 0/1 */
                int rl = detach_if_cached(gen_expr(e->l));
                int rr = gen_expr(e->r);
                vtype_t tl = infer(e->l), tr = infer(e->r);
                int isf = (tl == T_FLOAT || tr == T_FLOAT);
                if (isf) { if (tl != T_FLOAT) out("I2F R%d, R%d\n", rl, rl); if (tr != T_FLOAT) out("I2F R%d, R%d\n", rr, rr); }
                if (isf) out("CMPF R%d, R%d\n", rl, rr); else out("CMP R%d, R%d\n", rl, rr);
                int rres = reg_alloc(); int lset = newlab(), lend = newlab();
                out("MOV32 R%d, 0\n", rres);
                const char* jmp = (e->op == OP_LT) ? "BLTI" : (e->op == OP_GT) ? "BGTI"
                                 : (e->op == OP_LE) ? "BLEI" : (e->op == OP_GE) ? "BGEI"
                                 : (e->op == OP_EQ) ? "BEQI" : "BNEI";
                out("%s L%d\n", jmp, lset);
                out("BI L%d\n", lend);
                out("L%d: MOV32 R%d, 1\n", lset, rres);
                out("L%d:\n", lend);
                reg_free(rl); reg_free(rr);
                return rres;
            }
            if (e->op == OP_AND || e->op == OP_OR) {  /* 逻辑 */
                int rl = detach_if_cached(gen_bool(e->l));
                int rr = gen_bool(e->r);
                int rres = reg_alloc();
                out("%s R%d, R%d, R%d\n", (e->op == OP_AND) ? "AND" : "OR", rres, rl, rr);
                reg_free(rl); reg_free(rr);
                return rres;
            }
            /* 算术 + - * / %：结果写入新分配的临时寄存器，绝不覆盖源操作数
             * （源可能是被跨语句保持的局部变量寄存器，覆盖会破坏该变量的值） */
            int rl = detach_if_cached(gen_expr(e->l));
            int rr = gen_expr(e->r);
            vtype_t tl = infer(e->l), tr = infer(e->r);
            int isf = (tl == T_FLOAT || tr == T_FLOAT);
            if (isf) { if (tl != T_FLOAT) out("I2F R%d, R%d\n", rl, rl); if (tr != T_FLOAT) out("I2F R%d, R%d\n", rr, rr); }
            const char* m;
            if (isf) m = (e->op == OP_ADD) ? "ADDF" : (e->op == OP_SUB) ? "SUBF"
                     : (e->op == OP_MUL) ? "MULF" : "DIVF";
            else      m = (e->op == OP_ADD) ? "ADD" : (e->op == OP_SUB) ? "SUB"
                     : (e->op == OP_MUL) ? "MUL" : (e->op == OP_DIV) ? "DIV" : "MOD";
            int rt = reg_alloc();
            out("%s R%d, R%d, R%d\n", m, rt, rl, rr);
            reg_free(rl); reg_free(rr);
            return rt;
        }
    }
    int r = reg_alloc(); out("MOV32 R%d, 0\n", r); return r;
}

/* 把任意表达式物化为 0/1 布尔值 */
static int gen_bool(expr_t* e) {
    int r = gen_expr(e);
    int rt = reg_alloc();
    int l1 = newlab(), l2 = newlab();
    out("MOV32 R%d, 0\n", rt);
    out("CMP8 R%d, 0\n", r);
    out("BNEI L%d\n", l1);
    out("BI L%d\n", l2);
    out("L%d: MOV32 R%d, 1\n", l1, rt);
    out("L%d:\n", l2);
    reg_free(r);
    return rt;
}

/* ===================== 语句生成 ===================== */
static void gen_assign_dot(stmt_t* s) {
    int off; char csname[32]; vtype_t ft;
    int reg = resolve_dot(s->lhs_expr, &off, csname, &ft);
    if (s->compound == 2) {  /* 字段 ++ */
        int r = reg_alloc(); out("LDR R%d, R%d, %d\n", r, reg, off);
        int z = reg_alloc(); out("MOV32 R%d, 1\n", z); out("ADD R%d, R%d, R%d\n", r, r, z); reg_free(z);
        out("STR R%d, R%d, %d\n", r, reg, off); reg_free(r);
    } else if (s->compound == 1) {  /* 字段 += */
        int r = reg_alloc(); out("LDR R%d, R%d, %d\n", r, reg, off);
        int rv = gen_expr(s->e); vtype_t vt = infer(s->e);
        if (ft == T_FLOAT || vt == T_FLOAT) {
            if (ft != T_FLOAT) out("I2F R%d, R%d\n", r, r);
            if (vt != T_FLOAT) out("I2F R%d, R%d\n", rv, rv);
            out("ADDF R%d, R%d, R%d\n", r, r, rv);
        } else { out("ADD R%d, R%d, R%d\n", r, r, rv); }
        reg_free(rv);
        out("STR R%d, R%d, %d\n", r, reg, off); reg_free(r);
    } else {  /* 字段 = */
        int rv = gen_expr(s->e); vtype_t vt = infer(s->e);
        if (ft == T_FLOAT && vt != T_FLOAT) out("I2F R%d, R%d\n", rv, rv);
        else if (ft != T_FLOAT && vt == T_FLOAT) out("F2I R%d, R%d\n", rv, rv);
        out("STR R%d, R%d, %d\n", rv, reg, off); reg_free(rv);
    }
    if (reg != VM_FP) reg_free(reg);
}
static void gen_stmt(stmt_t* s);
static void gen_stmt_list(stmt_t* s) { while (s) { gen_stmt(s); s = s->next; } }

/*
 * @brief  生成一条语句的指令（AST 语句 -> 汇编）。
 * @param  stmt_t* s  语句节点
 * @return 无（void）
 * @note   按 s->kind 分派：S_DECL 分配槽位并可带初值、S_ASSIGN 生成赋值
 *         （compound 区分 = / += / ++）、S_IF 用标签实现条件跳转与 else、
 *         S_WHILE/S_DOWHILE/S_FOR 用"条件标签 + 跳转"实现循环、
 *         S_PRINT 调用 SYSCALL、S_RETURN 生成返回值与 RET、S_BLOCK 递归子语句。
 *         控制流全部通过 newlab() 生成标签、由 assembler 回填地址。 */
static void gen_stmt(stmt_t* s) {
    switch (s->kind) {
        case S_DECL: {
            sym_t* sy = lookup(sp_get(s->name_off));
            if (!sy) {
                /* 局部变量必然已由 assign_decls 登记进 LSYM；若缺失说明登记有漏洞。
                 * 必须报错而非静默跳过——否则不为该声明生成任何指令，后续读写
                 * 该变量会落到非法地址，产出「编译通过但运行崩溃」的程序。 */
                comp_error(0, COMP_ERR_INTERNAL, "变量 %s 未登记到符号表", sp_get(s->name_off));
                break;
            }
            if (sy->is_struct) break;   /* 结构体局部变量无需标量初始化；全局在 prologue 分配句柄 */
            if (sy->is_handle) {
                int r = reg_alloc();
                int size = sy->arr > 0 ? sy->arr * 4 : 64;
                if (s->init && s->init->kind == E_STR) size = s->init->ival * 4 + 4;
                out("ALLOC R%d, 0, %d\n", r, size);
                if (s->init && s->init->kind == E_STR) gen_str_fill(r, sp_get(s->init->name_off), s->init->ival);
                emit_store(sy, r); reg_free(r);
            } else if (s->init) {
                int r = gen_expr(s->init); vtype_t vt = infer(s->init);
                if (sy->type == T_FLOAT && vt != T_FLOAT) out("I2F R%d, R%d\n", r, r);
                else if (sy->type != T_FLOAT && vt == T_FLOAT) out("F2I R%d, R%d\n", r, r);
                if (sy->is_global || sy->is_handle || sy->is_struct) { emit_store(sy, r); reg_free(r); }
                else lv_bind(sy, r);   /* 局部标量：常驻，不写回内存 */
            }
            break;
        }
        case S_ASSIGN: {
            if (s->lhs_expr) { gen_assign_dot(s); break; }
            sym_t* sy = lookup(sp_get(s->name_off));
            if (!sy) {
                /* 局部变量必然已由 assign_decls 登记进 LSYM；若缺失说明登记有漏洞。
                 * 必须报错而非静默跳过——否则不为该声明生成任何指令，后续读写
                 * 该变量会落到非法地址，产出「编译通过但运行崩溃」的程序。 */
                comp_error(0, COMP_ERR_INTERNAL, "变量 %s 未登记到符号表", sp_get(s->name_off));
                break;
            }
            if (s->lhs_arr) {
                /* 数组元素赋值：rh = 数组基址，rt = 下标 × 4（字节偏移）。
                 * 必须按 compound 分流——原先这里只实现了 '='（直接存右值），
                 * 导致 a[i] += e 丢掉原值（实测 a[0]=5 后 a[0]+=3 得到 3 而非 8），
                 * 而 a[i]++ 的 s->e 为 NULL，gen_expr(NULL) 直接访问违规崩溃。 */
                int rh = reg_alloc(); emit_load(sy, rh);
                int ri = gen_expr(s->lhs_idx);
                int rt = reg_alloc(); out("SHLI R%d, R%d, 2\n", rt, ri);

                if (s->compound == 2) {          /* a[i]++ ：读旧值 → +1 → 写回 */
                    int rv = reg_alloc(); out("LDRH R%d, R%d, R%d\n", rv, rh, rt);
                    int z = reg_alloc();
                    if (sy->type == T_FLOAT) { out("MOV32 R%d, %uu\n", z, (unsigned)fbits(1.0f)); out("ADDF R%d, R%d, R%d\n", rv, rv, z); }
                    else { out("MOV32 R%d, 1\n", z); out("ADD R%d, R%d, R%d\n", rv, rv, z); }
                    reg_free(z);
                    out("STRH R%d, R%d, R%d\n", rv, rh, rt);
                    reg_free(rh); reg_free(ri); reg_free(rt); reg_free(rv);
                    break;
                }

                int rv;
                if (s->compound == 1) {          /* a[i] += e ：读旧值 → +e → 写回 */
                    int ro = reg_alloc(); out("LDRH R%d, R%d, R%d\n", ro, rh, rt);
                    int rr = gen_expr(s->e); vtype_t vt = infer(s->e);
                    int isf = (sy->type == T_FLOAT || vt == T_FLOAT);
                    if (isf) { if (sy->type != T_FLOAT) out("I2F R%d, R%d\n", ro, ro); if (vt != T_FLOAT) out("I2F R%d, R%d\n", rr, rr); }
                    out("%s R%d, R%d, R%d\n", isf ? "ADDF" : "ADD", ro, ro, rr);
                    if (isf && sy->type != T_FLOAT) out("F2I R%d, R%d\n", ro, ro);
                    reg_free(rr);
                    rv = ro;
                } else {                          /* a[i] = e ：直接存右值 */
                    rv = gen_expr(s->e); vtype_t vt = infer(s->e);
                    if (sy->type == T_FLOAT && vt != T_FLOAT) out("I2F R%d, R%d\n", rv, rv);
                    else if (sy->type != T_FLOAT && vt == T_FLOAT) out("F2I R%d, R%d\n", rv, rv);
                }
                out("STRH R%d, R%d, R%d\n", rv, rh, rt);
                reg_free(rh); reg_free(ri); reg_free(rt); reg_free(rv);
                break;
            }
            if (s->compound == 2) {  /* ++ */
                if (sy->is_global || sy->is_handle || sy->is_struct) {
                    int rl = reg_alloc(); emit_load(sy, rl);
                    int z = reg_alloc();
                    if (sy->type == T_FLOAT) { out("MOV32 R%d, %uu\n", z, (unsigned)fbits(1.0f)); out("ADDF R%d, R%d, R%d\n", rl, rl, z); }
                    else { out("MOV32 R%d, 1\n", z); out("ADD R%d, R%d, R%d\n", rl, rl, z); }
                    reg_free(z); emit_store(sy, rl); reg_free(rl);
                } else {
                    int rl = lv_get(sy);
                    int z = reg_alloc();
                    if (sy->type == T_FLOAT) { out("MOV32 R%d, %uu\n", z, (unsigned)fbits(1.0f)); out("ADDF R%d, R%d, R%d\n", rl, rl, z); }
                    else { out("MOV32 R%d, 1\n", z); out("ADD R%d, R%d, R%d\n", rl, rl, z); }
                    reg_free(z); lv_bind(sy, rl);   /* 常驻，不写回内存/不释放 */
                }
            } else if (s->compound == 1) {  /* += */
                if (sy->is_global || sy->is_handle || sy->is_struct) {
                    int rl = reg_alloc(); emit_load(sy, rl);
                    int rr = gen_expr(s->e); vtype_t vt = infer(s->e);
                    int isf = (sy->type == T_FLOAT || vt == T_FLOAT);
                    if (isf) { if (sy->type != T_FLOAT) out("I2F R%d, R%d\n", rl, rl); if (vt != T_FLOAT) out("I2F R%d, R%d\n", rr, rr); }
                    out("%s R%d, R%d, R%d\n", isf ? "ADDF" : "ADD", rl, rl, rr); reg_free(rr);
                    if (isf && sy->type != T_FLOAT) out("F2I R%d, R%d\n", rl, rl);
                    emit_store(sy, rl); reg_free(rl);
                } else {
                    int rl = lv_get(sy);
                    int rr = gen_expr(s->e); vtype_t vt = infer(s->e);
                    int isf = (sy->type == T_FLOAT || vt == T_FLOAT);
                    if (isf) { if (sy->type != T_FLOAT) out("I2F R%d, R%d\n", rl, rl); if (vt != T_FLOAT) out("I2F R%d, R%d\n", rr, rr); }
                    out("%s R%d, R%d, R%d\n", isf ? "ADDF" : "ADD", rl, rl, rr); reg_free(rr);
                    if (isf && sy->type != T_FLOAT) out("F2I R%d, R%d\n", rl, rl);
                    lv_bind(sy, rl);   /* 常驻，不写回/不释放 */
                }
            } else {  /* = */
                int rv = gen_expr(s->e); vtype_t vt = infer(s->e);
                if (sy->type == T_FLOAT && vt != T_FLOAT) out("I2F R%d, R%d\n", rv, rv);
                else if ((sy->type == T_INT || sy->type == T_CHAR || sy->type == T_SHORT) && vt == T_FLOAT) out("F2I R%d, R%d\n", rv, rv);
                if (sy->is_global || sy->is_handle || sy->is_struct) { emit_store(sy, rv); reg_free(rv); }
                else lv_bind(sy, rv);   /* 局部标量常驻，不写回内存 */
            }
            break;
        }
        case S_PRINT: {
            int r = gen_expr(s->e); vtype_t t = infer(s->e);
            if (t == T_STR) {
                /* 长度头 = 堆[句柄 + 0]：偏移 0 直接用零寄存器 VM_ZR，
                 * 无需再借寄存器装常量 0（与 S_INPUT 写长度头的写法保持一致）。 */
                int rl = reg_alloc();
                out("LDRH R%d, R%d, R%d\n", rl, r, VM_ZR);
                out("MOV R0, R%d\n", r);
                out("MOV R1, R%d\n", rl);
                out("SYSCALL %d\n", VM_SC_PRINT_STR);
                out("SYSCALL %d\n", VM_SC_PRINT_NEWLINE);
                reg_free(rl); reg_free(r);
            } else if (t == T_CHAR) {
                out("MOV R0, R%d\n", r);
                out("SYSCALL %d\n", VM_SC_PRINT_CHAR);
                out("SYSCALL %d\n", VM_SC_PRINT_NEWLINE);
                reg_free(r);
            } else {
                out("MOV R0, R%d\n", r);
                out("SYSCALL %d\n", (t == T_FLOAT) ? VM_SC_PRINT_FLOAT : VM_SC_PRINT_INT);
                out("SYSCALL %d\n", VM_SC_PRINT_NEWLINE);
                reg_free(r);
            }
            break;
        }
        case S_INPUT: {
            sym_t* sy = lookup(sp_get(s->name_off));
            if (!sy) {
                /* 局部变量必然已由 assign_decls 登记进 LSYM；若缺失说明登记有漏洞。
                 * 必须报错而非静默跳过——否则不为该声明生成任何指令，后续读写
                 * 该变量会落到非法地址，产出「编译通过但运行崩溃」的程序。 */
                comp_error(0, COMP_ERR_INTERNAL, "变量 %s 未登记到符号表", sp_get(s->name_off));
                break;
            }
            if (sy->type == T_STR) {
                int rh = reg_alloc(); out("ALLOC R%d, 0, %d\n", rh, 4 + 256 * 4);
                out("MOV R0, R%d\n", rh);
                out("MOV32 R1, 255\n");
                out("SYSCALL %d\n", VM_SC_INPUT_STR);
                out("STRH R0, R%d, R%d\n", rh, VM_ZR);   /* 写长度前缀（偏移 0） */
                emit_store(sy, rh); reg_free(rh);
            } else if (sy->type == T_FLOAT) {
                out("SYSCALL %d\n", VM_SC_INPUT_FLOAT);
                int r = reg_alloc(); out("MOV R%d, R0\n", r);
                if (sy->is_global || sy->is_handle || sy->is_struct) { emit_store(sy, r); reg_free(r); } else lv_bind(sy, r);
            } else if (sy->type == T_CHAR) {
                out("SYSCALL %d\n", VM_SC_INPUT_CHAR);
                int r = reg_alloc(); out("MOV R%d, R0\n", r);
                if (sy->is_global || sy->is_handle || sy->is_struct) { emit_store(sy, r); reg_free(r); } else lv_bind(sy, r);
            } else {
                out("SYSCALL %d\n", VM_SC_INPUT_INT);
                int r = reg_alloc(); out("MOV R%d, R0\n", r);
                if (sy->is_global || sy->is_handle || sy->is_struct) { emit_store(sy, r); reg_free(r); } else lv_bind(sy, r);
            }
            break;
        }
        case S_RETURN: {
            if (s->e) {
                int r = gen_expr(s->e); vtype_t vt = infer(s->e);
                if (cur_ret == T_FLOAT && vt != T_FLOAT) out("I2F R%d, R%d\n", r, r);
                else if (cur_ret != T_FLOAT && vt == T_FLOAT) out("F2I R%d, R%d\n", r, r);
                out("MOV R0, R%d\n", r); reg_free(r);
            }
            emit_ret();   /* 返回前冲刷所有常驻局部变量回内存 */
            break;
        }
        case S_IF: {
            int lelse = newlab(), lend = newlab();
            int rc = gen_bool(s->cond);
            out("CMP8 R%d, 1\n", rc);
            if (s->els) out("BNEI L%d\n", lelse); else out("BNEI L%d\n", lend);
            gen_stmt(s->body);
            if (s->els) { out("BI L%d\n", lend); out("L%d:\n", lelse); gen_stmt(s->els); }
            out("L%d:\n", lend);
            reg_free(rc);
            break;
        }
        case S_WHILE: {
            int lstart = newlab(), lend = newlab();
            lv_touch_stmt(s);   /* 预取循环体+条件所有局部标量：回边 lv_get 命中，不 LDR */
            out("L%d:\n", lstart);
            int rc = gen_bool(s->cond);
            out("CMP8 R%d, 1\n", rc); out("BNEI L%d\n", lend);
            gen_stmt(s->body);
            out("BI L%d\n", lstart);
            out("L%d:\n", lend);
            reg_free(rc);
            break;
        }
        case S_DOWHILE: {
            int lstart = newlab(), lend = newlab();
            lv_touch_stmt(s);   /* 预取循环体+条件所有局部标量：回边 lv_get 命中，不 LDR */
            out("L%d:\n", lstart);
            gen_stmt(s->body);
            int rc = gen_bool(s->cond);
            out("CMP8 R%d, 1\n", rc); out("BEQI L%d\n", lstart);
            out("L%d:\n", lend);
            reg_free(rc);
            break;
        }
        case S_FOR: {
            int lstart = newlab(), lcont = newlab(), lend = newlab();
            if (s->init_s) gen_stmt(s->init_s);
            lv_touch_stmt(s);   /* 预取循环体+条件所有局部标量：回边 lv_get 命中，不 LDR */
            out("L%d:\n", lstart);
            if (s->cond) { int rc = gen_bool(s->cond); out("CMP8 R%d, 1\n", rc); out("BNEI L%d\n", lend); reg_free(rc); }
            gen_stmt(s->body);
            out("L%d:\n", lcont);
            if (s->step_s) gen_stmt(s->step_s);
            out("BI L%d\n", lstart);
            out("L%d:\n", lend);
            break;
        }
        case S_BLOCK:
            gen_stmt_list(s->body);
            break;
        case S_EXPR:
            if (s->e) { int r = gen_expr(s->e); reg_free(r); }
            break;
    }
}

/* ===================== 函数元信息构建 ===================== */
static int compute_local_bytes(stmt_t* s) {
    int bytes = 0;
    while (s) {
        if (s->kind == S_DECL) bytes += (s->is_struct ? struct_size(sp_get(s->sname_off)) : 4);
        if (s->body)   bytes += compute_local_bytes(s->body);
        if (s->els)    bytes += compute_local_bytes(s->els);
        if (s->init_s) bytes += compute_local_bytes(s->init_s);
        if (s->step_s) bytes += compute_local_bytes(s->step_s);
        s = s->next;
    }
    return bytes;
}
static void assign_decls(stmt_t* s) {
    while (s) {
        if (s->kind == S_DECL) {
            /* 同上：LSYM 定长，超限报错，避免越界写 */
            if (!lsym_reserve(NLSM + 1)) {
                comp_error(0, COMP_ERR_INTERNAL, "函数内局部变量数超过上限 %d",
                           LSYM_CAP);
                break;
            }
            sym_t* sym = &LSYM[NLSM++]; pal_memset(sym, 0, sizeof(*sym));
            sym->name_off = s->name_off;;
            sym->type = s->type; sym->is_global = 0; sym->is_param = 0; sym->cached_reg = -1;
            if (s->is_struct) {
                sym->is_struct = 1; sym->sname_off = s->sname_off;;
                sym->arr = 0; sym->is_handle = 0;
                sym->off = LBASE; LBASE += struct_size(sp_get(s->sname_off));
            } else {
                sym->arr = s->arr_sz; sym->is_handle = (s->arr_sz > 0 || s->type == T_STR);
                sym->off = LBASE; LBASE += 4;
            }
        }
        if (s->body)   assign_decls(s->body);
        if (s->els)    assign_decls(s->els);
        if (s->init_s) assign_decls(s->init_s);
        if (s->step_s) assign_decls(s->step_s);
        s = s->next;
    }
}
static vtype_t infer_return(stmt_t* s) {
    vtype_t r = T_INT;
    while (s) {
        if (s->kind == S_RETURN && s->e && infer(s->e) == T_FLOAT) r = T_FLOAT;
        if (s->body)   { vtype_t b = infer_return(s->body);   if (b == T_FLOAT) r = T_FLOAT; }
        if (s->els)    { vtype_t b = infer_return(s->els);    if (b == T_FLOAT) r = T_FLOAT; }
        if (s->init_s) { vtype_t b = infer_return(s->init_s); if (b == T_FLOAT) r = T_FLOAT; }
        if (s->step_s) { vtype_t b = infer_return(s->step_s); if (b == T_FLOAT) r = T_FLOAT; }
        s = s->next;
    }
    return r;
}

static fmeta_t* find_fmeta(const char* nm) {
    for (int i = 0; i < NFMETA; i++) if (!pal_strcmp(FMETA[i].name, nm)) return &FMETA[i];
    return NULL;
}

/*
 * @brief  生成一个函数的完整汇编（前导 + 函数体 + 收尾）。
 * @param  func_t* f  函数节点
 * @return 无（void）
 * @note   流程：登记函数元信息（fmeta）→ 建立局部符号表并计算帧偏移
 *         → 发射 func_xxx: 标签 → 生成参数与局部变量 → 逐条生成语句
 *         → 以 RET 收尾。CALL 时需要的 frame_imm（帧大小）由
 *         compute_local_bytes 算出并存入 fmeta，供调用方发射 CALL 使用。
 *         函数入口标签由 assembler 解析，故此处无需关心绝对地址。 */
static void gen_function(func_t* f) {
    fmeta_t* m = find_fmeta(sp_get(f->name_off));
    /* 元信息必然存在（FMETA 已登记所有函数，见 codegen 的建表循环）。
     * 若缺失说明前面登记有漏洞——必须报错，不能静默降级：否则参数不会
     * 存入帧、LBASE 也算错，会产出「编译通过但运行错误」的程序。 */
    if (!m) {
        comp_error(0, COMP_ERR_INTERNAL, "函数 %s 缺少元信息", sp_get(f->name_off));
        return;
    }
    cur_ret = m->ret;
    out("func_%s:\n", sp_get(f->name_off));
    /* 函数序言：把参数存入帧 */
    for (int i = 0; i < m->nparam; i++)
        out("STR R%d, R%d, %d\n", i + 1, VM_FP, 8 + i * 4);
    /* 构建局部符号表 */
    /* 必须在复位 NLSM 之前清空：只清上一个函数真正占用的那些槽。
     * （旧代码固定清 256 项，在动态 LSYM 上会读写越界 → 访问违例。） */
    for (int i = 0; i < NLSM; i++) LSYM[i].cached_reg = -1;
    NLSM = 0;
    reg_init();   /* 每个函数独立使用寄存器池：重置空闲池，消除跨函数临时寄存器泄漏导致的污染 */
    g_cached_mask = 0;
    g_dirty_mask = 0;
   /* 清除上一函数的常驻状态 */
    LBASE = 8 + m->nparam * 4;
    for (int i = 0; i < m->nparam; i++) {
        /* 同上：参数也登记进 LSYM，需防溢出 */
        if (!lsym_reserve(NLSM + 1)) {
            comp_error(0, COMP_ERR_INTERNAL, "参数+局部变量数超过上限 %d",
                       LSYM_CAP);
            break;
        }
        sym_t* sym = &LSYM[NLSM++]; pal_memset(sym, 0, sizeof(*sym));
        sym->name_off = f->pname_off[i];
        sym->type = m->ptype[i]; sym->is_global = 0; sym->is_param = 1;
        sym->arr = 0; sym->is_handle = 0; sym->off = 8 + i * 4;
        sym->cached_reg = -1;   /* 必须显式置 -1：memset 留下的是 0，而 0 表示「常驻在 R0」 */
    }
    assign_decls(f->body);
    gen_stmt_list(f->body);
    emit_ret();   /* 函数自然结束前冲刷所有常驻局部变量回内存 */
}

/* ===================== 顶层入口 ===================== */
/*
 * @brief  代码生成总入口：AST -> VM 文本汇编。
 * @param  prog_t* p     AST 根节点
 * @param  char* out_buf 输出缓冲（调用方分配）
 * @param  int cap       out_buf 容量（字节）
 * @param  int* pool1    输出：1 字节池槽数
 * @param  int* pool2    输出：2 字节池槽数
 * @param  int* pool4    输出：4 字节池槽数
 * @return int           实际写入的字符数（与 cap 比较可判断是否被截断）
 * @note   流程：初始化输出与寄存器 → 登记结构体定义 → 建立全局符号表并分配
 *         各静态池槽位 → 第一遍扫描收集函数元信息（返回类型/帧大小）
 *         → 发射全局前导（初始化各池）→ CALL main → HALT
 *         → 逐个生成函数体 → 返回总长度。
 *         ?若返回长度 >= cap-1 说明汇编被截断（out() 已安全钳制），
 *         调用方必须报错而非产出残缺镜像。 */
int codegen(prog_t* p, char* out_buf, int cap, int* pool1, int* pool2, int* pool4) {
    G_OUT = out_buf; G_CAP = cap; G_OFF = 0; G_LAB = 0;
    reg_init();
    G_NSDEF = 0;
    /* 只有 NGSYM 是"没人管"的计数器：NLSM 在 gen_function 开头重置、NFMETA
     * 在登记函数元信息之前重置，唯独 NGSYM 从不清。实测表明这不会算错
     * （sp_free 使旧 name_off 失效，lookup 基本命中不到旧条目），但会让
     * GSYM 无限增长、lookup 的线性扫描越来越慢。清零消除这两个代价，
     * 顺带让三个计数器语义一致。保留 GSYM/LSYM 已扩容的容量，不重新分配。 */
    NGSYM  = 0;
    NLSM   = 0;
    NFMETA = 0;
    for (int i = 0; i < p->nstruct; i++) G_SDEFS[G_NSDEF++] = p->structs[i];

    /* 全局符号表 */
    int p1 = 0, p2 = 0, p4 = 0;
    for (stmt_t* g = p->globals; g; g = g->next) {
        /* 防符号表溢出：GSYM 是定长数组，超限必须报错并停止登记，
         * 否则会越界写坏相邻内存（静默产生难以排查的错误）。 */
        if (!gsym_reserve(NGSYM + 1)) {
            comp_error(0, COMP_ERR_INTERNAL, "全局变量数超过上限 %d",
                       GSYM_CAP);
            break;
        }
        sym_t* s = &GSYM[NGSYM++]; pal_memset(s, 0, sizeof(*s));
        s->name_off = g->name_off;
        s->type = g->type; s->is_global = 1; s->is_param = 0; s->arr = g->arr_sz;
        s->cached_reg = -1;   /* 必须显式置 -1：memset 留下的是 0，等于「常驻在 R0」 */
        if (g->is_struct) {
            s->is_struct = 1; s->sname_off = g->sname_off;
            s->is_handle = 1; s->off = p4 * 4; p4++;
        } else if (g->arr_sz > 0 || g->type == T_STR) {
            s->is_handle = 1; s->off = p4 * 4; p4++;
        } else if (g->type == T_CHAR) {
            s->is_handle = 0; s->pool = 1; s->off = p1; p1++;
        } else if (g->type == T_SHORT) {
            s->is_handle = 0; s->pool = 2; s->off = p2 * 2; p2++;
        } else {
            s->is_handle = 0; s->pool = 4; s->off = p4 * 4; p4++;
        }
    }

    /* 函数元信息 */
    NFMETA = 0;
    for (int i = 0; i < p->nfunc; i++) {
        func_t* f = p->funcs[i];
        fmeta_t* m = &FMETA[NFMETA++];
        pal_strncpy(m->name, sp_get(f->name_off), 32); m->nparam = f->nparam;
        for (int j = 0; j < f->nparam; j++) m->ptype[j] = f->ptype[j];
        /* 局部变量总字节（递归统计嵌套块/if/for 内的声明） */
        int lb = compute_local_bytes(f->body);
        /* 帧 = 参数 + 局部 + 溢出区(13 个临时寄存器 R6..R18 caller-save) */
        m->frame_imm = f->nparam * 4 + lb + 13 * 4;
        m->ret = infer_return(f->body);
    }

    /* 全局前导：初始化带初值的全局变量（在 PC=0 处运行） */
    for (stmt_t* g = p->globals; g; g = g->next) {
        sym_t* s = lookup(sp_get(g->name_off));
        if (!g->init) {
            if (s && s->is_struct) {
                int r = reg_alloc();
                out("ALLOC R%d, 0, %d\n", r, struct_size(sp_get(g->sname_off)));
                emit_store(s, r); reg_free(r);
            } else if (s && s->is_handle && s->arr > 0) {
                /* 全局数组（无字面量初值）：在全局前导里 ALLOC 一块缓冲并把句柄存回池槽，
                 * 否则 s->off 处恒为 0，a[i] 读写会落到非法句柄进而写坏堆边界。
                 * 逻辑与 S_DECL 局部数组一致（见 gen_stmt 的 S_DECL 分支）。 */
                int r = reg_alloc();
                out("ALLOC R%d, 0, %d\n", r, s->arr * 4);
                emit_store(s, r); reg_free(r);
            }
            continue;
        }
        /* 显式判 g->init：虽然上面 `if (!g->init) continue;` 已保证非 NULL，
         * 但依赖「前面 continue 过」是隐式契约；显式判空更抗改动。 */
        if (s->is_handle && g->init && g->init->kind == E_STR) {
            int r = gen_str_literal(g->init); emit_store(s, r); reg_free(r);
        } else if (s->is_handle) {
            if (g->type == T_STR) { int r = reg_alloc(); out("ALLOC R%d, 0, %d\n", r, 64); emit_store(s, r); reg_free(r); }
            /* 数组带初值：parser 已明确报错（见 parse_stmt 的声明分支），
             * 因此编译会因错误计数非零而中止，正常到不了这里；此处仅为兜底跳过。 */
        } else {
            int r = gen_expr(g->init); vtype_t vt = infer(g->init);
            if (s->type == T_FLOAT && vt != T_FLOAT) out("I2F R%d, R%d\n", r, r);
            else if (s->type != T_FLOAT && vt == T_FLOAT) out("F2I R%d, R%d\n", r, r);
            emit_store(s, r); reg_free(r);
        }
    }

    /* 入口桩：CALL main -> HALT */
    fmeta_t* mm = find_fmeta("main");
    /* 同上：装地址用借来的寄存器。入口桩不传参，故原本用 R6 也不会冲突，
     * 但保持与 E_CALL 一致的写法，避免将来给 main 增加参数时踩到同一个坑。 */
    int ra_main = reg_alloc();
    out("MOV32 R%d, func_main\n", ra_main);
    out("CALL R%d, %d\n", ra_main, mm ? mm->frame_imm : 0);
    reg_free(ra_main);
    out("HALT\n");

    /* 各函数（标签后可达） */
    for (int i = 0; i < p->nfunc; i++) gen_function(p->funcs[i]);

    *pool1 = p1; *pool2 = p2; *pool4 = p4;
    return G_OFF;
}
