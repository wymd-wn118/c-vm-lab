/******************************************************************************
 * 文件名:   WIN_std.c
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-7-25
 * 描述:     Windows 平台 PAL 实现。
 *          本目录（PAL/windows/）豁免「仅 C99」约束，允许使用 <string.h> 等
 *          C 库；其余工程代码必须改走 PAL.h 的 pal_* 接口。
 * 版本:     0.0.1
 * 版权:     无
 ******************************************************************************/
#include "WIN_std.h"
#include "PAL.h"
#include <windows.h>   /* GetConsoleOutputCP/SetConsoleOutputCP/GetConsoleCP/SetConsoleCP */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
/* 配置 */


/* Windows 下直接用 C 库实现，行为与原 <string.h>/<stdlib.h>/<ctype.h>/<stdio.h> 一致 */
void pal_memset(void* dst, int val, uint32_t n) { memset(dst, val, (size_t)n); }
void pal_memcpy(void* dst, const void* src, uint32_t n) { memcpy(dst, src, (size_t)n); }

/* float 与 uint32 位级互转（union 双关在主流编译器上可靠；亦可改 memcpy） */
float pal_u32_to_f32(uint32_t bits)
{
    union { uint32_t u; float f; } x;
    x.u = bits;
    return x.f;
}

uint32_t pal_f32_to_u32(float f)
{
    union { uint32_t u; float f; } x;
    x.f = f;
    return x.u;
}

/* 格式化输出：转发到 C 库 vprintf / printf */
void pal_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
void pal_vprintf(const char* fmt, va_list ap)
{
    vprintf(fmt, ap);
}

/* ===================== 内存分配（转发 libc 堆） ===================== */
void* pal_malloc(uint32_t size) { return malloc((size_t)size); }
void  pal_free(void* p) { free(p); }
void* pal_realloc(void* p, uint32_t size) { return realloc(p, (size_t)size); }
void* pal_calloc(uint32_t n, uint32_t size) { return calloc((size_t)n, (size_t)size); }

/* ===================== 字符串（转发 libc） ===================== */
int pal_strcmp(const char* a, const char* b) { return strcmp(a, b); }
char* pal_strncpy(char* dst, const char* src, uint32_t n) { return strncpy(dst, src, (size_t)n); }
uint32_t pal_strlen(const char* s) { return (uint32_t)strlen(s); }
char* pal_strdup(const char* s)
{
    uint32_t n = (uint32_t)strlen(s);
    char* d = (char*)malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}

/* ===================== 字符分类（转发 libc ctype） ===================== */
int pal_isalpha(int c) { return isalpha(c); }
int pal_isalnum(int c) { return isalnum(c); }

/* ===================== 数值解析（转发 libc） ===================== */
long pal_strtol(const char* s, char** end, int base) { return strtol(s, end, base); }
unsigned long pal_strtoul(const char* s, char** end, int base) { return strtoul(s, end, base); }
int pal_atoi(const char* s) { return atoi(s); }
double pal_atof(const char* s) { return atof(s); }

/* ===================== 格式化拼接（转发 libc vsnprintf） ===================== */
int pal_vsnprintf(char* buf, uint32_t size, const char* fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)size, fmt, ap);
}

/* ===================== 文件 IO（整文件读写，转发 libc stdio） ===================== */
char* pal_read_file(const char* path, uint32_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[sz] = 0;
    *out_size = (uint32_t)sz;
    return buf;
}

int pal_write_file(const char* path, const void* data, uint32_t size)
{
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    if (size > 0 && fwrite(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int pal_remove(const char* path)
{
    /* 成功（文件已删除）返回 0，否则 -1。编译失败时必须删掉过期产物，
     * 否则后续加载会静默运行上一个版本的程序。 */
    return (remove(path) == 0) ? 0 : -1;
}

/* ===================== 控制台编码 =====================
 * 源码与字符串字面量都是 UTF-8，而中文 Windows 控制台默认 936(GBK)，
 * 不切换就会把 UTF-8 字节按 GBK 显示，中文全部变花。
 * 同时把输入代码页也切到 UTF-8，这样用户输入中文文件名也能被 scanf 正确解码。 */

static UINT g_saved_out_cp = 0;   /* 切换前的输出代码页 */
static UINT g_saved_in_cp  = 0;   /* 切换前的输入代码页 */
static int  g_cp_saved     = 0;   /* 是否已保存过（避免重复保存覆盖原值） */

void pal_console_utf8(void)
{
    if (!g_cp_saved) {
        g_saved_out_cp = GetConsoleOutputCP();
        g_saved_in_cp  = GetConsoleCP();
        g_cp_saved     = 1;
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

void pal_console_restore(void)
{
    if (g_cp_saved) {
        SetConsoleOutputCP(g_saved_out_cp);
        SetConsoleCP(g_saved_in_cp);
        g_cp_saved = 0;
    }
}

/* ===================== 控制台输入 ===================== */

#include <conio.h>   /* _kbhit / _getch：Windows CRT 自带的非阻塞输入 */

/* 阻塞读一行。契约：返回的字符串不含行尾 \r\n，调用方可直接 strcmp。
 * 顺带剥掉行首 UTF-8 BOM —— 某些终端/管道会在流开头塞一个。 */
char* pal_gets(char* buf, uint32_t size)
{
    size_t n;
    char*  p;

    if (size == 0) return NULL;
    if (!fgets(buf, (int)size, stdin)) return NULL;

    n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;

    p = buf;
    if ((unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) {
        char* d = buf;
        char* s = buf + 3;
        while ((*d++ = *s++) != 0) { }
    }
    return buf;
}

/* 非阻塞：只检测，不消费。长任务运行期间用它判断"用户是否想打断"。 */
int pal_input_ready(void)
{
    return _kbhit() ? 1 : 0;
}

/* 非阻塞：读一个已就绪的字符；无输入返回 -1（绝不等待）。 */
int pal_getch_now(void)
{
    return _kbhit() ? (int)(unsigned char)_getch() : -1;
}
