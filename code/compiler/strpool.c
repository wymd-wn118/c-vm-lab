/******************************************************************************
 * 瀛楃涓叉睜瀹炵幇锛岃 strpool.h銆? ******************************************************************************/
#include "strpool.h"
#include "error.h"

char*     g_strpool = NULL;
uint32_t  g_sp_len  = 0;
uint32_t  g_sp_cap  = 0;

/* 鍒濆瀹归噺锛氭寜灏忔簮鐮佷及绠楋紱涓嶅鏃?sp_add 鍐呴儴缈诲€嶆墿瀹广€?*/
#define SP_INIT_CAP 1024u
#define SP_MAX_CAP  (1u << 20)   /* 1 MB 纭笂闄愪繚鎶わ紙MCU 涓婁篃涓嶄細鏃犻檺娑級 */

void sp_init(void) {
    g_sp_cap = SP_INIT_CAP;
    g_strpool = (char*)comp_malloc(g_sp_cap);
    if (!g_strpool) return;   /* 已 OOM：宁可让池保持不可用，也不让
                               * sp_add() 往 NULL 指针上写。 */
    if (!g_strpool) {                     /* 鍒嗛厤澶辫触锛氫繚鎸?涓嶅彲鐢?鐘舵€?*/
        g_sp_cap = 0;
        g_sp_len = 0;
        return;
    }
    g_strpool[0] = '\0';   /* 鍋忕Щ 0 鎭掍负绌轰覆锛屾案涔呬繚鐣?*/
    g_sp_len = 1;          /* 宸插崰鐢?1 瀛楄妭锛涘悗缁?sp_add 浠庡亸绉?1 寮€濮嬪啓锛?                              鍚﹀垯绗竴涓覆浼氳惤鍦ㄥ亸绉?0 涓娿€佽 sp_get(0) 褰撴垚绌轰覆鑰屽彇涓嶅洖 */
}

void sp_free(void) {
    if (g_strpool) comp_free(g_strpool);
    g_strpool = NULL;
    g_sp_len = 0;
    g_sp_cap = 0;
}

/* 纭繚姹犺兘瀹圭撼 need 瀛楄妭锛堝惈缁撳熬 '\0'锛夛紝涓嶅鍒欑炕鍊嶆墿瀹广€?*/
static int sp_reserve(uint32_t need) {
    if (need <= g_sp_cap) return 1;
    uint32_t cap = g_sp_cap;
    while (cap < need) {
        uint32_t ncap = cap * 2u;
        if (ncap < cap) { cap = need; break; }   /* 婧㈠嚭淇濇姢 */
        cap = ncap;
    }
    if (cap > SP_MAX_CAP) cap = SP_MAX_CAP;
    if (cap < need) return 0;                    /* 瓒呯‖涓婇檺 */
    char* p = (char*)comp_realloc(g_strpool, g_sp_cap, cap);
    if (!p) return 0;
    g_strpool = p;
    g_sp_cap = cap;
    return 1;
}

uint32_t sp_add(const char* s) {
    if (!s || !g_strpool) return 0;
    int n = (int)pal_strlen(s);
    return sp_add_n(s, n);
}

/* 鍦ㄦ睜涓嚎鎬ф煡鎵炬槸鍚﹀凡瀛樺湪鍐呭鐩稿悓鐨勪覆锛屽懡涓繑鍥炲叾鍋忕Щ锛屽惁鍒欒繑鍥?0銆? * 浠呰 sp_add_n 璋冪敤锛岀敤浜庡幓閲嶏紙鍚屼竴鍐呭鍙瓨涓€浠斤級銆? * 澶嶆潅搴?O(鍞竴涓叉暟 脳 涓查暱)锛氱紪璇戞湡闈炵儹璺緞锛屼笖鍞竴涓查€氬父浠呭嚑鐧句釜锛屼唬浠峰彲鎺ュ彈锛? * 鐩告瘮鍝堝笇琛ㄧ渷涓嬫《鏁扮粍鍐呭瓨涓庡疄鐜板鏉傚害鈥斺€擬CU 涓婄┖闂存瘮杩欏嚑鍗佹绉掓洿鐝嶈吹銆?*/
static uint32_t sp_find_dup(const char* s, int n) {
    uint32_t off = 1;                    /* 璺宠繃鍋忕Щ 0 鐨勪繚鐣欑┖涓?*/
    while (off < g_sp_len) {
        const char* e = g_strpool + off;
        int elen = (int)pal_strlen(e);
        /* 鍏堢敤闀垮害杩囨护锛堝鏁颁覆闀垮害鍗充笉鍚岋級锛屽啀鐢ㄩ瀛楃 O(1) 杩囨护锛?         * 鏈€鍚庢墠閫愬瓧鑺傛瘮杈冦€傛敞鎰忥細s 鍙兘涓嶄互 '\0' 缁撳熬锛坰p_add_n 鏀寔瀹氶暱鐗囨锛夛紝
         * 鏁呬笉鑳界敤 pal_strcmp锛堜細瓒婄晫璇伙級锛屾澶勫彧姣旇緝缁欏畾鐨?n 瀛楄妭銆?*/
        if (elen == n && e[0] == s[0]) {
            int i = 1;
            while (i < n && e[i] == s[i]) i++;
            if (i == n) return off;
        }
        off += (uint32_t)elen + 1u;
    }
    return 0;
}

uint32_t sp_add_n(const char* s, int n) {
    if (!g_strpool) return 0;
    if (n <= 0) return 0;                          /* 绌轰覆 -> 鍋忕Щ 0 */
    /* 鍘婚噸锛氭睜涓凡鏈夌浉鍚屽唴瀹瑰垯鐩存帴澶嶇敤鍏跺亸绉伙紝涓嶅啀杩藉姞鍓湰 */
    uint32_t dup = sp_find_dup(s, n);
    if (dup) return dup;
    /* 鐜版湁鍐呭 g_sp_len 瀛楄妭 + 鏂颁覆 n 瀛楄妭 + '\0' */
    uint32_t need = g_sp_len + (uint32_t)n + 1u;
    if (!sp_reserve(need)) {
        /* 鎵╁澶辫触锛氭姤璇婃柇骞堕€€鍥炵┖涓诧紝閬垮厤鍚庣画瓒婄晫銆?*/
        comp_error(0, COMP_ERR_INTERNAL, "string pool exhausted");
        return 0;
    }
    pal_memcpy(g_strpool + g_sp_len, s, (uint32_t)n);
    g_sp_len += (uint32_t)n;
    g_strpool[g_sp_len] = '\0';
    uint32_t off = g_sp_len;     /* 璁板綍鏈覆璧峰鍋忕Щ锛堟寚鍚戜覆棣栵紝涓嬩竴涓瓧鑺傛槸鍘?'\0' 浣嶇疆锛?*/
    g_sp_len += 1u;              /* 鍗犳帀缁撳熬 '\0' */
    return off - (uint32_t)n;    /* 杩斿洖涓查鍋忕Щ */
}

const char* sp_get(uint32_t off) {
    if (!g_strpool || off == 0 || off >= g_sp_len) return "";
    return g_strpool + off;
}
