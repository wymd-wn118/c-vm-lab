/******************************************************************************
 * 瀛楃涓叉睜锛坰tring pool锛夛細缂栬瘧鍣ㄧ粺涓€瀛楃涓插瓨鍌ㄣ€? *
 * 鍔ㄦ満锛圫TM32 绉绘 / MCU 鑷妇锛夛細
 *   鍘熷厛 token_t 涓?AST 鑺傜偣锛坋xpr_t/stmt_t/func_t/struct_def_t锛夊悇鑷唴宓? *   char[32] 瀹氶暱鏁扮粍锛岀粷澶у鏁?token/鑺傜偣鏍规湰鐢ㄤ笉鍒?32 瀛楄妭锛岄€犳垚澶ч噺娴垂
 *   锛堜箣鍓嶉噺鍖栵細姣忎釜 expr_t 160B 閲岀害 96B 鏄┖姘旓紱struct_def_t 2212B 鍑犱箮
 *   鍏ㄦ槸 fname[32][32]/fsname[32][32] 鐨勫畾闀挎暟缁勶級銆? *
 *   鏀逛负缁熶竴杩涗竴涓叏灞€瀛楃姹狅紝鑺傜偣鍙瓨 4 瀛楄妭鍋忕Щ uint32_t锛屽瓧绗︿覆鎸夐渶杩藉姞銆? *   鏀剁泭锛? *     1) 姣忚妭鐐瑰瓧绗︿覆瀛楁浠?32B脳N 闄嶅埌 4B脳N锛? *     2) 瀛楃涓查暱搴︿笉鍐嶅彈 31 瀛楃闄愬埗锛圱K_STR_MAX 鐥涚偣娑堝け锛夛紱
 *     3) 姹犲湪缂栬瘧瀹屾垚鍚庡彲鏁翠綋 comp_free锛岄厤鍚堝垎鏃跺鐢ㄧ瓥鐣?B銆? *
 * 鐢熷懡鍛ㄦ湡锛氱敱 compiler_main 鍦?lex 涔嬪墠 sp_init锛宔mit 闀滃儚涔嬪悗 sp_free銆? *   鏈熼棿 lexer/parser/codegen 浠绘剰闃舵閮藉彲 sp_add / sp_get銆? ******************************************************************************/
#ifndef STRPOLL_H_
#define STRPOLL_H_

#include "compiler.h"

/* 鍏ㄥ眬瀛楃涓叉睜锛堜互 '\0' 鍒嗛殧鐨勬墎骞冲瓧绗︽祦锛夈€備粎渚涘唴閮ㄨ闂紝澶栭儴鐢?sp_* 鎺ュ彛銆?*/
extern char*     g_strpool;
extern uint32_t  g_sp_len;   /* 宸茬敤瀛楄妭鏁帮紙鍚粨灏?'\0'锛?*/
extern uint32_t  g_sp_cap;   /* 瀹归噺 */

/* 鍒濆鍖?/ 閲婃斁銆俿p_free 鍚?g_strpool 缃?NULL锛屽彲瀹夊叏鍐嶆 sp_init銆?*/
void sp_init(void);
void sp_free(void);

/* 杩藉姞涓€涓互 '\0' 缁撳熬鐨勫瓧绗︿覆锛岃繑鍥炲叾鍦ㄦ睜涓殑鍋忕Щ銆? * 绌烘寚閽堟垨绌轰覆杩斿洖 0锛堝亸绉?0 鎭掍负姹犱腑棣栦釜 '\0'锛岀瓑浠蜂簬绌轰覆锛夈€?*/
uint32_t sp_add(const char* s);

/* 杩藉姞瀹氶暱瀛楃涓诧紙s[0..n-1]锛夛紝鑷姩琛?'\0'銆傜敤浜庡凡鐭ラ暱搴︾殑鐗囨銆?*/
uint32_t sp_add_n(const char* s, int n);

/* 鐢卞亸绉诲彇鍥炲彧璇绘寚閽堛€俹ff==0 鎴栬秺鐣岃繑鍥炴寚鍚戠┖涓茬殑鎸囬拡銆?*/
const char* sp_get(uint32_t off);

#endif /* STRPOLL_H_ */
