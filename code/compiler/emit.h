#ifndef EMIT_H_
#define EMIT_H_
#include "compiler.h"
#include "vm_image.h"   /* vm_static_layout_t / VM_IMG_* 镜像契约 */

/* 写镜像文件（见 compiler_design.md §8.2）：
 *   magic + 布局表(9×u32) + code_len + data_len + code + 静态数据(全 0)
 * 成功返回 0。pool1/pool2/pool4 为各静态池被全局变量「占用」的槽数；
 * max_frame_size 为编译期全局最大调用栈帧字节数（含 ret+fp 头），供 loader 算精确栈窗口；
 * 发射时池总大小 = 占用数 + VM_IMG_POOL*_PAD（留运行时余量）。 */
int emit_image(const char* path, const uint8_t* code, int code_len,
               int pool1_used, int pool2_used, int pool4_used, int max_frame_size);

/* 把镜像构建到调用者提供的缓冲（不碰文件系统）。
 *   > 0 : 实际写入字节数（成功）
 *   < 0 : -N = 需要的字节数（缓冲不足，或探测调用 out=NULL/out_cap=0）
 *   -3  : 总长度溢出 int
 * `out` 必须是宿主内存，不能来自 comp_malloc()——结束时 comp_reap_all()
 * 会清空编译器的 MM 堆，指向其中的指针会悬空。 */
int emit_image_to_buf(uint8_t* out, uint32_t out_cap,
                      const uint8_t* code, int code_len,
                      int pool1_used, int pool2_used, int pool4_used,
                      int max_frame_size);

/* -S：直接把汇编文本打到 stdout */
void emit_asm_text(const char* asm_text);

#endif /* EMIT_H_ */
