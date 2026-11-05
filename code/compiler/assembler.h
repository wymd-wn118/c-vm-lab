#ifndef ASSEMBLER_H_
#define ASSEMBLER_H_
#include "compiler.h"

/* 文本汇编 -> 字节。成功返回 0，写入 out（容量 cap），*out_len 为产出字节数，
 * *out_max_frame 为全局最大调用栈帧字节数（含 ret+fp 头，即 8 + max(frame_imm)）。
 * 失败返回负错误码：-1 未知助记符 / -2 输出缓冲不足 / -3 操作数不足 /
 * -4 寄存器解析失败 / -5 标签未定义 / -6 内存不足（文本拆分或行副本分配失败）。
 * 所有失败路径均已释放本次分配的中间资源，不会泄漏。
 * 支持标签（name:）与两遍扫描回填；立即数按 VM 大端序发射。 */
int assemble(const char* asm_text, uint8_t* out, int cap, int* out_len, int* out_max_frame);

/* assemble 返回码 -> 可读消息（供调用方呈现具体诊断，而非笼统的"assembly failed"）。
 * 返回静态字符串，调用方不可修改、无需释放。 */
const char* asm_err_msg(int rc);

#endif /* ASSEMBLER_H_ */
