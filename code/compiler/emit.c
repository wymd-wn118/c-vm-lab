/******************************************************************************
 * 发射器：把汇编/字节产出写出。
 * 镜像格式（与 compiler_design.md §8.2 / vm_image.h 一致，头为宿主端字节序）：
 *   magic     u32 = 'OSV1'
 *   pool1_cnt u32   ( = pool1_used + VM_IMG_POOL1_PAD )
 *   pool2_cnt u32
 *   pool4_cnt u32
 *   static_pool1_used u32
 *   static_pool2_used u32
 *   static_pool4_used u32
 *   static_varlen_len u32
 *   n_fixed_handles   u32
 *   code_len  u32
 *   data_len  u32            = 1B池 + 2B池 + 4B池 + 静态变长区
 *   code : code_len 字节（vm_load 直接载）
 *   data : data_len 字节（[1B池][2B池][4B池][静态变长区]，首版初值由 code 前导写，此段全 0）
 ******************************************************************************/
#include "emit.h"

/**
 * @brief 把输出镜像存入调用者提供的缓冲区（不碰文件系统）
 *
 * @param out               缓冲区本体，由调用者申请；必须是宿主内存，
 *                          不能来自 comp_malloc（编译结束时 comp_reap_all 会清空 MM 堆）
 * @param out_cap           缓冲区容量（字节）
 * @param code              代码段
 * @param code_len          代码段长度
 * @param pool1_used        1B池使用量
 * @param pool2_used        2B池使用量
 * @param pool4_used        4B池使用量
 * @param max_frame_size    最大栈帧大小
 * @return int              > 0 : 实际写入的字节数（成功）
 *                          < 0 : -N 表示需要 N 字节（缓冲区不足）
 *                          -3  : 总长度超出 int 表示范围
 * @note  传 out=NULL、out_cap=0 即为「探测」调用，只回报所需长度、不写任何字节。
 */
int32_t emit_image_to_buf(uint8_t* out, uint32_t out_cap,
                      const uint8_t* code, int code_len,
                      int pool1_used, int pool2_used, int pool4_used,
                      int max_frame_size) {
    vm_static_layout_t l;
    l.pool1_cnt         = (uint32_t)pool1_used + VM_IMG_POOL1_PAD;
    l.pool2_cnt         = (uint32_t)pool2_used + VM_IMG_POOL2_PAD;
    l.pool4_cnt         = (uint32_t)pool4_used + VM_IMG_POOL4_PAD;
    l.static_pool1_used = (uint32_t)pool1_used;
    l.static_pool2_used = (uint32_t)pool2_used;
    l.static_pool4_used = (uint32_t)pool4_used;
    l.static_varlen_len = 0;
    l.n_fixed_handles   = 0;
    l.max_frame_size    = (uint32_t)max_frame_size;

    uint32_t cl       = (uint32_t)code_len;
    uint32_t data_len = vm_img_data_len(&l);

    /* 用 64 位算：32 位相加可能回绕成小值，从而让下面的容量检查失效 */
    uint64_t total64 = 48ull + (uint64_t)cl + (uint64_t)data_len;
    if (total64 > 0x7FFFFFFFull) return -3;
    uint32_t total = (uint32_t)total64;

    /* ★ 唯一的越界防线，必须位于【所有写入】之前。缓冲区不足时一个字节都不写。 */
    if (!out || total > out_cap) return -(int)total;

    /* 头：magic(4) + 9 个布局 u32(36) + code_len(4) + data_len(4) = 48 字节 */
    uint32_t hdr[12];
    hdr[0] = VM_IMG_MAGIC;
    hdr[1] = l.pool1_cnt; hdr[2] = l.pool2_cnt; hdr[3] = l.pool4_cnt;
    hdr[4] = l.static_pool1_used; hdr[5] = l.static_pool2_used;
    hdr[6] = l.static_pool4_used; hdr[7] = l.static_varlen_len;
    hdr[8] = l.n_fixed_handles;   hdr[9] = l.max_frame_size;
    hdr[10] = cl; hdr[11] = data_len;

    pal_memcpy(out, hdr, 48);
    pal_memcpy(out + 48, code, cl);
    pal_memset(out + 48 + cl, 0, data_len);      /* 数据段全 0 */

    return (int)total;
}

/**
 * @brief 把输出镜像写入文件
 *
 * @param path              输出文件路径
 * @param code              代码段
 * @param code_len          代码段长度
 * @param pool1_used        1B池使用量
 * @param pool2_used        2B池使用量
 * @param pool4_used        4B池使用量
 * @param max_frame_size    最大栈帧大小
 * @return int              成功返回 0；分配失败返回 -1；
 *                          写文件失败返回 pal_write_file 的错误码
 * @note  先「探测」所需长度再按需分配：与 emit_image_to_buf 共用同一份布局计算，
 *        也不会在 100 KB 的 MM 堆里多占无用空间。
 */
int emit_image(const char* path, const uint8_t* code, int code_len,
               int pool1_used, int pool2_used, int pool4_used, int max_frame_size) {
    int need = emit_image_to_buf(NULL, 0, code, code_len,
                                 pool1_used, pool2_used, pool4_used, max_frame_size);
    if (need >= 0) return -1;                    /* 理论上不可达 */
    uint32_t total = (uint32_t)(-need);

    uint8_t* buf = (uint8_t*)comp_malloc(total);
    if (!buf) return -1;

    int n = emit_image_to_buf(buf, total, code, code_len,
                              pool1_used, pool2_used, pool4_used, max_frame_size);
    if (n < 0) { comp_free(buf); return -1; }    /* 理论上不可达 */

    int rc = pal_write_file(path, buf, (uint32_t)n);
    comp_free(buf);
    return rc;
}

void emit_asm_text(const char* asm_text) {
    pal_printf("%s", asm_text);
}
