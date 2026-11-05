/******************************************************************************
 * 文件名:   WIN_syscall.c
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-7-29
 * 描述:     Windows 平台 SYSCALL 默认实现（PAL 接口 pal_syscall）。
 *          由 VM 的 SYSCALL 指令调用，把 print/input 等语义落到宿主 I/O。
 *          未来新增 syscall 统一在此扩展，上层（main.c 等）无需注册回调。
 *          编号语义见 vm_isa.h 的 VM_SC_*。
 * 版本:     0.0.1
 * 版权:     无
 ******************************************************************************/
#include "WIN_std.h"
#include "PAL.h"
#include "VM.h"
#include "VM_MM.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* 输出捕获缓冲（测试/诊断用）：开启后 print 类 syscall 在写宿主的同时也累积到这里，
 * 供上层（如 main.c 的端到端比对）读取。关闭时仅写宿主。 */
#define PAL_CAP_MAX (1u << 16)
static char   g_cap[PAL_CAP_MAX];
static size_t g_caplen;
static int    g_cap_on;

void pal_capture_enable(int on) { g_cap_on = on ? 1 : 0; }
void pal_capture_reset(void)   { g_caplen = 0; g_cap[0] = 0; }
const char* pal_capture_get(void) { g_cap[g_caplen] = 0; return g_cap; }

/* 写一个字符：到宿主 stdout（windows 平台允许直接用 stdio）+ （若开启）捕获缓冲 */
static void pal_putch(char c)
{
    printf("%c", c);
    if (g_cap_on && g_caplen + 1 < PAL_CAP_MAX) g_cap[g_caplen++] = (char)c;
}
/* 写字符串 */
static void pal_puts(const char* s)
{
    while (*s) pal_putch(*s++);
}

/* SYSCALL 默认实现：编号语义见 vm_isa.h 的 VM_SC_*。
 * 新增 syscall 只需在此 switch 增加分支，上层无需改动。 */
void pal_syscall(struct vm_t* vm, uint32_t nr)
{
    uint32_t* R = vm->cpu.R;
    switch (nr)
    {
        case VM_SC_EXIT:
            /* 终止信号：由 VM 在 SYSCALL 指令后处理；此处默认无操作 */
            break;
        case VM_SC_PRINT_INT: {
            char b[16]; int n = sprintf(b, "%d", (int)R[0]);
            b[n] = 0; pal_puts(b);
            break;
        }
        case VM_SC_PRINT_FLOAT: {
            float f = pal_u32_to_f32(R[0]);
            char b[32]; int n = sprintf(b, "%g", f);
            b[n] = 0; pal_puts(b);
            break;
        }
        case VM_SC_PRINT_STR: {
            uint32_t h = R[0], len = R[1], v = 0;
            for (uint32_t i = 0; i < len; i++) {
                if (vm_mm_read(vm, h, 4 + i * 4, &v) == 0) pal_putch((char)(uint8_t)v);
            }
            break;
        }
        case VM_SC_PRINT_CHAR:
            pal_putch((char)(uint8_t)R[0]);
            break;
        case VM_SC_PRINT_NEWLINE:
            pal_putch('\n');
            break;
        case VM_SC_INPUT_INT: {
            int x = 0; if (scanf("%d", &x) == 1) R[0] = (uint32_t)x;
            break;
        }
        case VM_SC_INPUT_FLOAT: {
            float f = 0; if (scanf("%g", &f) == 1) R[0] = pal_f32_to_u32(f);
            break;
        }
        case VM_SC_INPUT_STR: {
            uint32_t h = R[0], maxlen = R[1]; char buf[512]; buf[0] = 0;
            if (scanf(" %511[^\n]", buf) == 1) {
                size_t l = strlen(buf); if (l > maxlen) l = maxlen;
                for (uint32_t i = 0; i < l; i++) {
                    uint32_t v = (uint8_t)buf[i];
                    vm_mm_write(vm, h, 4 + i * 4, v);
                }
                R[0] = (uint32_t)l;
            } else R[0] = 0;
            break;
        }
        case VM_SC_INPUT_CHAR: {
            int c = getchar(); R[0] = (uint32_t)(unsigned char)c;
            break;
        }
        case VM_SC_FLUSH:
            fflush(stdout);
            break;
        default:
            /* 未知 syscall：忽略（可经 pal_printf 报警） */
            break;
    }
}
