/******************************************************************************
 * 文件名:   PAL_generic.c
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-7-25
 * 描述:     PAL 通用（非 Windows / 嵌入式）实现：纯 C99，不依赖任何平台库。
 *          由 PAL.c 在 PLATFORM != CONFIG_WINDOWS 时包含进来。
 * 版本:     0.0.1
 * 版权:     无
 ******************************************************************************/
#include "PAL.h"

/* 字节填充：逐字节写入，不依赖 <string.h> */
void pal_memset(void* dst, int val, uint32_t n)
{
    uint8_t* p = (uint8_t*)dst;
    uint8_t  v = (uint8_t)(val & 0xFF);
    for (uint32_t i = 0; i < n; i++) p[i] = v;
}

/* 字节拷贝：逐字节复制，不处理重叠区域（与 <string.h> 的 memcpy 语义一致） */
void pal_memcpy(void* dst, const void* src, uint32_t n)
{
    uint8_t*       d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/* uint32 位模式按 IEEE754 解释为 float（union 双关，纯 C99，无严格别名 UB） */
float pal_u32_to_f32(uint32_t bits)
{
    union { uint32_t u; float f; } x;
    x.u = bits;
    return x.f;
}

/* float 解释为 uint32 位模式 */
uint32_t pal_f32_to_u32(float f)
{
    union { uint32_t u; float f; } x;
    x.f = f;
    return x.u;
}

/* 格式化输出：通用后端默认空实现（嵌入式平台应替换为自身串口/日志输出）。
 * 编译宿主（Windows）使用 WIN_std.c 的真实实现。 */
void pal_printf(const char* fmt, ...)
{
    (void)fmt;
}
void pal_vprintf(const char* fmt, va_list ap)
{
    (void)fmt; (void)ap;
}

/* ===================== SYSCALL 默认实现 ===================== */
/* 通用后端（无 libc / 嵌入式）默认不实现具体 I/O：
 * 只提供捕获缓冲与相关符号，真正的 I/O 由平台实现
 * （windows / stm32）提供。 */
#define PAL_CAP_MAX_G (1u << 16)
static char   g_cap_g[PAL_CAP_MAX_G];
static size_t g_caplen_g;
static int    g_cap_on_g;

void pal_capture_enable(int on) { g_cap_on_g = on ? 1 : 0; }
void pal_capture_reset(void)   { g_caplen_g = 0; g_cap_g[0] = 0; }
const char* pal_capture_get(void) { g_cap_g[g_caplen_g] = 0; return g_cap_g; }

void pal_syscall(struct vm_t* vm, uint32_t nr)
{
    (void)vm; (void)nr;   /* 通用后端没有 I/O；输出交给平台实现 */
}

/* ===================== 控制台编码 =====================
 * 通用/嵌入式平台没有"控制台代码页"这个概念：
 *   - Linux 终端本身按 UTF-8 解析，无需切换；
 *   - MCU 的串口输出编码由串口工具决定。
 * 因此这里给空实现，让调用方无需为平台差异写 #if。 */
void pal_console_utf8(void)    { }
void pal_console_restore(void) { }

/* ===================== 控制台输入 =====================
 * 通用/嵌入式平台没有标准输入流，行输入需由串口/UART 中断把字符填进
 * 环形缓冲，再在平台实现里覆盖下面三个函数。这里给出安全的退化实现：
 *   - pal_gets 返回 NULL（表示无输入，CLI 会正常退出而不是死等）
 *   - pal_input_ready 恒 0（永远不打断长任务）
 *   - pal_getch_now 恒 -1
 * 这样即使没有输入设备，链接与运行都不会出错。 */
char* pal_gets(char* buf, uint32_t size)
{
    (void)size;
    if (buf) buf[0] = 0;
    return NULL;
}

int pal_input_ready(void) { return 0; }
int pal_getch_now(void)   { return -1; }
