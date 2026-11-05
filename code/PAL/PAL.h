/******************************************************************************
 * 文件名:   PAL.h
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-7-25
 * 描述:     PAL（Platform Abstraction Layer）接口声明。
 *          本文件只声明 pal_* 接口契约；具体实现由各平台子目录提供，
 *          由 PAL.c 按 PLATFORM 宏把对应的实现文件包含进来。
 * 版本:     0.0.1
 * 版权:     无
 ******************************************************************************/
#ifndef PAL_H_
#define PAL_H_

#include "config.h"
#include "stdint.h"
#include <stdarg.h>   /* pal_vprintf 需要 va_list */

/* ===========================================================================
 * 跨平台约束（全工程必须遵守）
 * ---------------------------------------------------------------------------
 * 除「测试用 main.c」与「PAL/<平台>/ 下的平台实现文件」外，工程其余所有
 * 代码只允许使用 C99，禁止直接或间接使用 <stdlib.h> / <string.h> 的 malloc /
 * free / memset / memcpy / memmove 等。任何「必须用到的库能力」都统一经本文件
 * 声明接口（pal_*），由具体平台在 PAL/windows/（或未来的 PAL/generic/、
 * PAL/stm32/ 等）实现。
 *
 *   - pal_memset / pal_memcpy         : 字节级内存填充 / 拷贝（替代 <string.h>）
 *   - pal_u32_to_f32 / pal_f32_to_u32 : uint32 <-> float 位级互转
 *     （替代「memcpy 类型双关」：既不依赖 <string.h>，也无严格别名 UB）
 *
 * 注意：本文件不含任何实现，也不再根据平台 include 具体实现头；实现的分发
 * 统一由 PAL.c 完成，避免本文件随平台增多而膨胀。
 * =========================================================================== */

/* 内存填充：把 dst 起 n 字节置为 val（仅取低 8 位） */
void pal_memset(void* dst, int val, uint32_t n);

/* 内存拷贝：src -> dst，共 n 字节（不处理重叠区域） */
void pal_memcpy(void* dst, const void* src, uint32_t n);

/* 把 uint32 位模式按 IEEE754 解释为 float */
float    pal_u32_to_f32(uint32_t bits);

/* 把 float 按 IEEE754 解释为 uint32 位模式 */
uint32_t pal_f32_to_u32(float f);

/* 格式化输出（诊断信息统一出口）。默认后端（windows）转发 <stdio.h> 的 printf /
 * vprintf；其余工程代码（含编译器报错模块）经此接口输出，不直接调用 printf，
 * 避免平台耦合。嵌入式平台应替换为自身串口 / 日志输出。 */
void pal_printf(const char* fmt, ...);
void pal_vprintf(const char* fmt, va_list ap);

/* ===================== SYSCALL（SYSCALL 指令） ===================== */
/* SYSCALL 的默认实现：由 VM 的 SYSCALL 指令调用，在宿主上完成
 * 打印/输入等语义。指令编号含义见 vm_isa.h 的 VM_SC_*。
 * 以后新增系统调用统一在此扩展；上层（main.c 等）无需注册回调，
 * 直接用这个 PAL 默认实现，避免逻辑散落各处。
 * 这里只前向声明 vm_t（不包含 VM.h），以保持 PAL 的低层抽象。 */
struct vm_t;
void pal_syscall(struct vm_t* vm, uint32_t nr);

/* 输出捕获（供测试/诊断）：开启后，打印类系统调用在写宿主的同时
 * 也累积到内部缓冲，便于上层做 diff / 回放。
 * 关闭则只写宿主。由各平台实现提供。 */
void pal_capture_enable(int on);
void pal_capture_reset(void);
const char* pal_capture_get(void);

/* ===================== 文件 IO（走 PAL，不用 <stdio.h>） ===================== */
/* 读整个文件到 pal_malloc 分配的内存；成功返回非空指针（文本模式补 '\0'），
 * *out_size 为字节数（不含结尾 '\0'）；失败返回 NULL。调用方用 pal_free 释放。
 * 替代 compiler_main.c / Loader.c 的 fopen+fread+fclose 整文件读。 */
char* pal_read_file(const char* path, uint32_t* out_size);
/* 写整个文件（二进制）；成功返回 0，失败返回非 0。替代 emit.c 的 fopen+fwrite。 */
int   pal_write_file(const char* path, const void* data, uint32_t size);

/* 删除文件。0 = 已删除（或本来就不存在），-1 = 删不掉。
 * 编译器用它清掉过期产物，避免构建失败后残留旧镜像，
 * 让后续加载误跑到上一个程序。 */
int   pal_remove(const char* path);

/* ===================== 内存分配（替代 <stdlib.h> 的 malloc/free/realloc/calloc） ===================== */
void*    pal_malloc(uint32_t size);
void     pal_free(void* p);
void*    pal_realloc(void* p, uint32_t size);
void*    pal_calloc(uint32_t n, uint32_t size);   /* 返回清零内存 */

/* ===================== 字符串（替代 <string.h> 的 str*） ===================== */
int      pal_strcmp(const char* a, const char* b);
char*    pal_strncpy(char* dst, const char* src, uint32_t n);  /* 标准 strncpy 语义（含零填充） */
uint32_t pal_strlen(const char* s);
char*    pal_strdup(const char* s);

/* ===================== 字符分类（替代 <ctype.h> 的 isalpha/isalnum） ===================== */
int      pal_isalpha(int c);
int      pal_isalnum(int c);

/* ===================== 数值解析（替代 <stdlib.h> 的 strtol/strtoul/atoi/atof） ===================== */
long          pal_strtol(const char* s, char** end, int base);
unsigned long pal_strtoul(const char* s, char** end, int base);
int           pal_atoi(const char* s);
double        pal_atof(const char* s);

/* ===================== 格式化拼接（替代 <stdio.h> 的 vsnprintf） ===================== */
int pal_vsnprintf(char* buf, uint32_t size, const char* fmt, va_list ap);

/* ===================== 控制台编码 =====================
 * 本工程源码与字符串字面量统一为 UTF-8，而中文 Windows 的控制台默认是
 * 936(GBK)：不切换的话 printf 会把 UTF-8 字节按 GBK 解释，中文全部变花。
 *
 * - Windows：调用 SetConsoleOutputCP/SetConsoleCP，并记住原值以便恢复；
 * - 其他平台：空实现（Linux 终端本身就是 UTF-8，MCU 由串口配置决定）。
 *
 * 用法：程序启动后、任何 printf 之前调用 pal_console_utf8()，
 *       退出前调用 pal_console_restore()（不恢复会污染同一窗口里的后续程序）。 */
void pal_console_utf8(void);
void pal_console_restore(void);

/* ===================== 控制台输入 =====================
 * 阻塞式：读完整一行（含行编辑，由平台库提供），成功返回 buf，EOF 返回 NULL。
 *   Windows -> fgets；嵌入式 -> 由串口中断填充行缓冲后覆盖本实现。
 * 非阻塞式：只做"检测 + 读单键"，立即返回、绝不等待。用于长任务运行期间
 *   让用户能按键打断（否则只能 Ctrl+C 杀掉整个进程）。
 *   Windows -> _kbhit / _getch；Linux -> select + read；MCU -> 查环形缓冲。
 *
 * 注意：pal_getch_now 只用于"打断信号"，不用于组命令行 —— 命令行仍交给
 * pal_gets，这样退格/回显等行编辑行为完全由平台库负责，CLI 不用自己实现。 */
char* pal_gets(char* buf, uint32_t size);   /* 阻塞读一行 */
int   pal_input_ready(void);                /* 1 = 有输入待读（非阻塞） */
int   pal_getch_now(void);                  /* 读一个已就绪的字符；无输入返回 -1 */


#endif
