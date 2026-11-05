#ifndef CODEGEN_H_
#define CODEGEN_H_
#include "compiler.h"
#include "parser.h"

/* 生成文本汇编（VM 汇编文本，见 assembler.c），写入 out（容量 cap）。
 * pool1/pool2/pool4 返回本程序用到的各静态池「槽数」（供镜像头填写）。
 * 返回写入的字符数。 */
int codegen(prog_t* p, char* out, int cap, int* pool1, int* pool2, int* pool4);

#endif /* CODEGEN_H_ */
