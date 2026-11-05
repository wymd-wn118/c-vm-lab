/******************************************************************************
 * 文件名:   VM.c
 * 作者:     蜗咏梦蝶
 * 创建时间: 2026-5-10
 * 修改时间: 2026-7-25
 * 描述:     虚拟机（单文件，内部按「派发表 + 处理函数」组织）
 * 版本:     0.0.3
 * 版权:     无
 ******************************************************************************/

#include "VM.h"
#include "VM_MM.h"
#include "PAL.h"   /* 字节拷贝/填充、float 位级互转、报表打印(pal_printf)统一走 PAL，禁用 <string.h>/<stdio.h> */

/* ===================== 物理存储 =====================
 * 真正的底层缓冲由 MM 统一管理（见 MM.c 的 g_phys_ram）。
 * VM 不持有也不分配内存：任务的 code/data/stack 窗口由 Loader 向 MM 申请后
 * 填入 vm_t（base 指向 MM 物理 RAM 内的偏移），VM 只负责按窗口执行指令。 */

/* 全局虚拟机实例 */
vm_t g_vm;

/* 宿主系统调用统一走 PAL 默认实现 pal_syscall（见 PAL.h / WIN_syscall.c），
 * 上层无需注册回调。 */

#define VM_DISPATCH_SIZE 64
#define VM_RUN_MAX_STEPS 100000000u

/* 编译期常量：是否启用 threaded（computed goto）派发，供 DEBUG 输出与结构复用 */
#if defined(__GNUC__) && !defined(VM_NO_THREADED_DISPATCH)
#define VM_THREADED 1
#else
#define VM_THREADED 0
#endif

/* ===================== 内部工具 ===================== */
/* 取指令流下一字节（大端解码的基础），自动推进 PC */
/*
 * @brief  取指令流下一字节（取指辅助）
 * @param  vm_t* vm  当前虚拟机实例
 * @return uint8_t   取出的 1 字节操作数/立即数
 * @note   自动推进 PC；VM 采用大端解码，所有取指经此函数。
 */
static uint8_t vm_rd_u8(vm_t* vm)
{
    return vm->prog.base[vm->cpu.R[VM_PC]++];
}

/* 取指令流 32 位立即数（大端），自动推进 PC */
/*
 * @brief  取指令流 32 位立即数（取指辅助）
 * @param  vm_t* vm  当前虚拟机实例
 * @return uint32_t  大端解码出的 4 字节立即数
 * @note   分 4 次调用 vm_rd_u8，自动推进 PC。
 */
static uint32_t vm_rd_u32(vm_t* vm)
{
    /* 小端原生取指：code 段多字节立即数/偏移以小端存放，pal_memcpy 直接 load。
     * x86/Cortex-M4 均为小端，编译为单条 mov；任意对齐安全、无严格别名 UB。 */
    uint32_t v;
    pal_memcpy(&v, vm->prog.base + vm->cpu.R[VM_PC], 4);
    vm->cpu.R[VM_PC] += 4;
    return v;
}

/* 数据读写统一经 VM_MM（句柄→指针→访问），见 vmh_ldr / vmh_str */

/* 入栈：成功返回 0，否则返回栈错误码 */
/*
 * @brief  向栈区压入一个 32 位字
 * @param  vm_t* vm      当前虚拟机实例
 * @param  uint32_t data 待压入的数据
 * @return uint8_t       0=成功，否则栈错误码（溢出）
 * @note   SP 先自增再写；栈区与程序/数据区物理分离（由 loader 分配窗口）。
 */
static uint8_t vm_push(vm_t* vm, uint32_t data)
{
    uint32_t sp = vm->cpu.R[VM_SP];
    if (sp + 4u > vm->stack.size) return VM_STACK_OVERFLOW_ERROR;
    *(uint32_t*)(vm->stack.base + sp) = data;
    vm->cpu.R[VM_SP] = sp + 4u;
    return 0;
}

/* 出栈：成功返回 0，否则返回栈错误码 */
/*
 * @brief  从栈区弹出一个 32 位字
 * @param  vm_t* vm       当前虚拟机实例
 * @param  uint32_t* out  接收弹出数据的输出指针
 * @return uint8_t        0=成功，否则栈错误码（下溢/溢出）
 * @note   SP 先读再自减；out 非空由调用方保证。
 */
static uint8_t vm_pop(vm_t* vm, uint32_t* out)
{
    uint32_t sp = vm->cpu.R[VM_SP];
    if (sp < 4u) return VM_STACK_UNDERFLOW_ERROR;
    sp -= 4u;
    *out = *(uint32_t*)(vm->stack.base + sp);
    vm->cpu.R[VM_SP] = sp;
    return 0;
}

/* 堆分配器已迁移至 VM_MM.c（见 VM_MM.h）：位图 + 1/2/4 固定池 + 句柄表 + 变长区 + GC */

/* (已迁移至 VM_MM.c) */

/* (已迁移至 VM_MM.c) */

/* (已迁移至 VM_MM.c) */

/* 处理函数统一返回错误码：0 成功；跳转类在内部直接改写 PC。
 * 约定：每个处理函数首行用 vm_rd_u8 跳过操作码，再按编码顺序读取操作数。 */
typedef uint8_t (*vm_op_handler)(vm_t* vm);

/* ===================== 移动 / 加载存储 ===================== */
/*
 * @brief  操作码 MOV：寄存器间移动 Rd = Rs
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_MOV Rd(1B) Rs(1B)
 *          - 直接操作寄存器文件，安全指令。
 */
static uint8_t vmh_mov(vm_t* vm)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint8_t rd = vm_rd_u8(vm);
    uint8_t rs = vm_rd_u8(vm);
    vm->cpu.R[rd] = vm->cpu.R[rs];
    return 0;
}

/*
 * @brief  操作码 MOV8：8 位立即数送寄存器 Rd = Imm(8)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_MOV8 Rd(1B) Imm(1B)
 */
static uint8_t vmh_mov8(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd  = vm_rd_u8(vm);
    uint8_t imm = vm_rd_u8(vm);
    vm->cpu.R[rd] = imm;
    return 0;
}

/*
 * @brief  操作码 MOV16：16 位立即数送寄存器 Rd = Imm(16)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_MOV16 Rd(1B) Imm(2B, 大端)
 */
static uint8_t vmh_mov16(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  rd  = vm_rd_u8(vm);
    uint32_t imm = vm_rd_u8(vm);
    imm |= ((uint32_t)vm_rd_u8(vm)) << 8;
    vm->cpu.R[rd] = imm;
    return 0;
}

/*
 * @brief  操作码 MOV32：32 位立即数送寄存器 Rd = Imm(32)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_MOV32 Rd(1B) Imm(4B, 大端)
 */
static uint8_t vmh_mov32(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  rd  = vm_rd_u8(vm);
    uint32_t imm = vm_rd_u32(vm);
    vm->cpu.R[rd] = imm;
    return 0;
}

/*
 * @brief  操作码 LDR：经句柄 Rs 加载堆中一个字到 Rd（Rd = handle_mem[Rs][Imm]，非裸地址）
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（越界时返回 VM_DATA_OVERFLOW_ERROR）
 * @note   汇编结构：VM_LDR Rd(1B) Rs(1B) Imm(4B)
 *          - 经由句柄 Rs 查 MM 落到物理地址，属"访问堆"指令，须校验边界。
 */
static uint8_t vmh_ldr(vm_t* vm)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  rs  = vm_rd_u8(vm);
    uint32_t off = vm_rd_u32(vm);
    if (rs == VM_FP) {                     /* 局部/参数/溢出区：帧内访问，直接走调用栈 */
        uint32_t fp = vm->cpu.R[VM_FP];
        /* 局部变量的 FP 相对访问用负偏移：[FP-4] 到达时 off = 0xFFFFFFFC，
         * 这是合法的。故用 64 位有符号计算并校验「结果」；旧的 addr + 4 > limit
         * 写法会在这类偏移上回绕，放行越界访问。 */
        int64_t a64 = (int64_t)fp + (int64_t)(int32_t)off;
        if (a64 < 0 || a64 + 4 > (int64_t)vm->stack.size) return VM_STACK_OVERFLOW_ERROR;
        uint32_t addr = (uint32_t)a64;
        if (addr > vm->cpu.R[VM_SP]) return VM_STACK_OVERFLOW_ERROR;
        vm->cpu.R[rd] = *(uint32_t*)(vm->stack.base + addr);
        return 0;
    }
    uint32_t val = 0;
    uint8_t err = vm_mm_read(vm, vm->cpu.R[rs], off, &val);
    if (err) return err;
    vm->cpu.R[rd] = val;
    return 0;
}

/*
 * @brief  操作码 STR：经句柄 Rs 把一个字写入堆（handle_mem[Rs][Imm] = Rd，非裸地址）
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（越界时返回 VM_DATA_OVERFLOW_ERROR）
 * @note   汇编结构：VM_STR Rd(1B) Rs(1B) Imm(4B)
 *          - 直接操作堆，不安全指令（越界由 VM_MM 拦截并返回错误码）。
 */
static uint8_t vmh_str(vm_t* vm)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  rs  = vm_rd_u8(vm);
    uint32_t off = vm_rd_u32(vm);
    if (rs == VM_FP) {                     /* 局部/参数/溢出区：帧内访问，直接写调用栈 */
        uint32_t fp = vm->cpu.R[VM_FP];
        /* 与 vmh_ldr 同理：局部变量本来就是负偏移，所以校验 64 位有符号地址，
         * 而不是拿 off 和 fp 直接比较。 */
        int64_t a64 = (int64_t)fp + (int64_t)(int32_t)off;
        if (a64 < 0 || a64 + 4 > (int64_t)vm->stack.size) return VM_STACK_OVERFLOW_ERROR;
        uint32_t addr = (uint32_t)a64;
        if (addr > vm->cpu.R[VM_SP]) return VM_STACK_OVERFLOW_ERROR;
        *(uint32_t*)(vm->stack.base + addr) = vm->cpu.R[rd];
        return 0;
    }
    return vm_mm_write(vm, vm->cpu.R[rs], off, vm->cpu.R[rd]);
}

/* ===================== 裸全内存加载存储（LDRX / STRX） =====================
 * 与 LDR/STR 不同，这里 Rs 是「裸基址」（如 R_POOL4），直接 data.base[Rs+Imm]，
 * 不经句柄表、可访问 data 区内任意字节（含 header/位图，信任编译器生成正确）。
 * 字节序与 vm_mm_read/write 一致（大端），边界按 data.size 检查。 */
/*
 * @brief  操作码 LDRX：从裸地址 data.base[Rs+Imm] 加载一个字到 Rd
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（越界返回 VM_DATA_OVERFLOW_ERROR）
 * @note   汇编结构：VM_LDRX Rd(1B) Rs(1B) Imm(4B)；裸全内存寻址，不经句柄表。
 */
static uint8_t vmh_ldrx(vm_t* vm)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  rs  = vm_rd_u8(vm);
    uint32_t off = vm_rd_u32(vm);
    uint64_t addr = (uint64_t)vm->cpu.R[rs] + off;   /* 64 位避免回绕误判 */
    if (addr + 4 > vm->data.size) return VM_DATA_OVERFLOW_ERROR;
    const uint8_t* b = vm->data.base + (uint32_t)addr;
    uint32_t val;
    pal_memcpy(&val, b, 4);   /* 小端裸内存读 */
    vm->cpu.R[rd] = val;
    return 0;
}

/*
 * @brief  操作码 STRX：把一个字写入裸地址 data.base[Rs+Imm] = Rd
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（越界返回 VM_DATA_OVERFLOW_ERROR）
 * @note   汇编结构：VM_STRX Rd(1B) Rs(1B) Imm(4B)；裸全内存寻址，不经句柄表。
 */
static uint8_t vmh_strx(vm_t* vm)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  rs  = vm_rd_u8(vm);
    uint32_t off = vm_rd_u32(vm);
    uint64_t addr = (uint64_t)vm->cpu.R[rs] + off;   /* 64 位避免回绕误判 */
    if (addr + 4 > vm->data.size) return VM_DATA_OVERFLOW_ERROR;
    uint8_t* b = vm->data.base + (uint32_t)addr;
    uint32_t val = vm->cpu.R[rd];
    pal_memcpy(b, &val, 4);   /* 小端裸内存写 */
    return 0;
}

/* ===================== 窄宽裸内存加载存储（LDRXB/H · STRXB/H） =====================
 * 与 LDRX/STRX（固定 4 字节）互补，真正按 1/2 字节访问 data 区，
 * 供 char(1B 池)/short(2B 池) 使用。加载对窄类型做符号扩展（char/short 默认有符号），
 * 与 C 语义一致；写入只落低 1/2 字节。 */
static uint8_t vmh_ldrxb(vm_t* vm)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  rs  = vm_rd_u8(vm);
    uint32_t off = vm_rd_u32(vm);
    uint64_t addr = (uint64_t)vm->cpu.R[rs] + off;
    if (addr + 1 > vm->data.size) return VM_DATA_OVERFLOW_ERROR;
    int8_t v = (int8_t)vm->data.base[(uint32_t)addr];
    vm->cpu.R[rd] = (uint32_t)(int32_t)v;   /* 符号扩展 */
    return 0;
}
static uint8_t vmh_strxb(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  rs  = vm_rd_u8(vm);
    uint32_t off = vm_rd_u32(vm);
    uint64_t addr = (uint64_t)vm->cpu.R[rs] + off;
    if (addr + 1 > vm->data.size) return VM_DATA_OVERFLOW_ERROR;
    vm->data.base[(uint32_t)addr] = (uint8_t)(vm->cpu.R[rd] & 0xFF);
    return 0;
}
static uint8_t vmh_ldrxh(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  rs  = vm_rd_u8(vm);
    uint32_t off = vm_rd_u32(vm);
    uint64_t addr = (uint64_t)vm->cpu.R[rs] + off;
    if (addr + 2 > vm->data.size) return VM_DATA_OVERFLOW_ERROR;
    const uint8_t* b = vm->data.base + (uint32_t)addr;
    uint16_t u; pal_memcpy(&u, b, 2); int16_t v = (int16_t)u;  /* 小端裸内存读 + 符号扩展 */
    vm->cpu.R[rd] = (uint32_t)(int32_t)v;   /* 符号扩展 */
    return 0;
}
static uint8_t vmh_strxh(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  rs  = vm_rd_u8(vm);
    uint32_t off = vm_rd_u32(vm);
    uint64_t addr = (uint64_t)vm->cpu.R[rs] + off;
    if (addr + 2 > vm->data.size) return VM_DATA_OVERFLOW_ERROR;
    uint8_t* b = vm->data.base + (uint32_t)addr;
    uint32_t val = vm->cpu.R[rd];
    uint16_t u = (uint16_t)val; pal_memcpy(b, &u, 2);   /* 小端裸内存写 */
    return 0;
}

/* ===================== 数值类型转换（I2F / F2I） =====================
 * 供显式 cast(int)/(float) 与隐式 int?float 混合运算使用。 */
static uint8_t vmh_i2f(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm);
    float f = (float)(int32_t)vm->cpu.R[ra];
    vm->cpu.R[rd] = pal_f32_to_u32(f);
    return 0;
}
static uint8_t vmh_f2i(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm);
    int32_t i = (int32_t)pal_u32_to_f32(vm->cpu.R[ra]);
    vm->cpu.R[rd] = (uint32_t)i;
    return 0;
}

/* ===================== 寄存器偏移堆访问（LDRH / STRH） =====================
 * 供数组 / 字符串等「句柄 + 动态下标」访问：Rh=句柄，Ro=寄存器偏移(字节)，
 * 经 vm_mm_read/write 访问堆块内部任意 4 字节元素。 */
static uint8_t vmh_ldrh(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), rh = vm_rd_u8(vm), ro = vm_rd_u8(vm);
    uint32_t val = 0;
    uint8_t err = vm_mm_read(vm, vm->cpu.R[rh], vm->cpu.R[ro], &val);
    if (err) return err;
    vm->cpu.R[rd] = val;
    return 0;
}
static uint8_t vmh_strh(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rs = vm_rd_u8(vm), rh = vm_rd_u8(vm), ro = vm_rd_u8(vm);
    return vm_mm_write(vm, vm->cpu.R[rh], vm->cpu.R[ro], vm->cpu.R[rs]);
}

/* ===================== 算术 / 逻辑 ===================== */
/*
 * @brief  操作码 ADD：普通加法 Rd = Ra + Rb
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_ADD Rd(1B) Ra(1B) Rb(1B)
 */
static uint8_t vmh_add(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] + vm->cpu.R[rb];
    return 0;
}

/*
 * @brief  操作码 ADDI：立即数加法 Rd = Ra + Imm(32)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_ADDI Rd(1B) Ra(1B) Imm(4B,大端)
 */
static uint8_t vmh_addi(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  ra  = vm_rd_u8(vm);
    uint32_t imm = vm_rd_u32(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] + imm;
    return 0;
}

/*
 * @brief  操作码 SUB：普通减法 Rd = Ra - Rb
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_SUB Rd(1B) Ra(1B) Rb(1B)
 */
static uint8_t vmh_sub(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] - vm->cpu.R[rb];
    return 0;
}

/*
 * @brief  操作码 SUBI：立即数减法 Rd = Ra - Imm(32)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_SUBI Rd(1B) Ra(1B) Imm(4B,大端)
 */
static uint8_t vmh_subi(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  ra  = vm_rd_u8(vm);
    uint32_t imm = vm_rd_u32(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] - imm;
    return 0;
}

/*
 * @brief  操作码 MUL：普通乘法 Rd = Ra * Rb
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_MUL Rd(1B) Ra(1B) Rb(1B)
 */
static uint8_t vmh_mul(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] * vm->cpu.R[rb];
    return 0;
}

/*
 * @brief  操作码 MULF：浮点乘法 Rd = Rs * Rt（位级 float 转换）
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_MULF Rd(1B) Rs(1B) Rt(1B)
 *          - 寄存器内以 uint32 存 IEEE754 位，经 PAL 的 pal_u32_to_f32 /
 *            pal_f32_to_u32 位级互转后再相乘（不依赖 <string.h>）。
 */
static uint8_t vmh_mulf(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), rs = vm_rd_u8(vm), rt = vm_rd_u8(vm);
    float a = pal_u32_to_f32(vm->cpu.R[rs]);
    float b = pal_u32_to_f32(vm->cpu.R[rt]);
    a *= b;
    vm->cpu.R[rd] = pal_f32_to_u32(a);
    return 0;
}

/*
 * @brief  操作码 DIV：普通除法 Rd = Ra / Rb
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（Rb==0 返回 VM_DIV_ZERO_ERROR）
 * @note   汇编结构：VM_DIV Rd(1B) Ra(1B) Rb(1B)
 */
static uint8_t vmh_div(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    if (vm->cpu.R[rb] == 0) return VM_DIV_ZERO_ERROR;
    {
        /* 有符号除法：CMP 按有符号比较，'/' 也必须一致（旧实现按无符号，
         * 导致 -7/2 == 2147483644）。另：INT32_MIN / -1 在 x86 上会触发异常，
         * 这里显式回绕成补码结果。 */
        int32_t a = (int32_t)vm->cpu.R[ra];
        int32_t b = (int32_t)vm->cpu.R[rb];
        if (a == INT32_MIN && b == -1) vm->cpu.R[rd] = (uint32_t)a;
        else                          vm->cpu.R[rd] = (uint32_t)(a / b);
    }
    return 0;
}

/*
 * @brief  操作码 DIVF：浮点除法 Rd = Rs / Rt（位级 float 转换）
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_DIVF Rd(1B) Rs(1B) Rt(1B)
 */
static uint8_t vmh_divf(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), rs = vm_rd_u8(vm), rt = vm_rd_u8(vm);
    float a = pal_u32_to_f32(vm->cpu.R[rs]);
    float b = pal_u32_to_f32(vm->cpu.R[rt]);
    a /= b;
    vm->cpu.R[rd] = pal_f32_to_u32(a);
    return 0;
}

/* 浮点加 / 减（与 MULF/DIVF 同一套位级转换） */
/*
 * @brief  操作码 ADDF：浮点加法 Rd = Rs + Rt
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_ADDF Rd(1B) Rs(1B) Rt(1B)
 */
static uint8_t vmh_addf(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), rs = vm_rd_u8(vm), rt = vm_rd_u8(vm);
    float a = pal_u32_to_f32(vm->cpu.R[rs]);
    float b = pal_u32_to_f32(vm->cpu.R[rt]);
    float r = a + b;
    vm->cpu.R[rd] = pal_f32_to_u32(r);
    return 0;
}

/*
 * @brief  操作码 SUBF：浮点减法 Rd = Rs - Rt
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_SUBF Rd(1B) Rs(1B) Rt(1B)
 */
static uint8_t vmh_subf(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), rs = vm_rd_u8(vm), rt = vm_rd_u8(vm);
    float a = pal_u32_to_f32(vm->cpu.R[rs]);
    float b = pal_u32_to_f32(vm->cpu.R[rt]);
    float r = a - b;
    vm->cpu.R[rd] = pal_f32_to_u32(r);
    return 0;
}

/*
 * @brief  操作码 MOD：普通取余 Rd = Ra % Rb
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（Rb==0 返回 VM_MOD_ZERO_ERROR）
 * @note   汇编结构：VM_MOD Rd(1B) Ra(1B) Rb(1B)
 */
static uint8_t vmh_mod(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    if (vm->cpu.R[rb] == 0) return VM_MOD_ZERO_ERROR;
    {
        /* 有符号取余，理由同 vmh_div（同时也避开
         * INT32_MIN % -1 的异常）。 */
        int32_t a = (int32_t)vm->cpu.R[ra];
        int32_t b = (int32_t)vm->cpu.R[rb];
        if (a == INT32_MIN && b == -1) vm->cpu.R[rd] = 0;
        else                          vm->cpu.R[rd] = (uint32_t)(a % b);
    }
    return 0;
}

/*
 * @brief  操作码 SHL：逻辑左移 Rd = Ra << Rb
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_SHL Rd(1B) Ra(1B) Rb(1B)
 */
static uint8_t vmh_shl(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] << vm->cpu.R[rb];
    return 0;
}

/*
 * @brief  操作码 SHR：逻辑右移 Rd = Ra >> Rb
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_SHR Rd(1B) Ra(1B) Rb(1B)
 */
static uint8_t vmh_shr(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] >> vm->cpu.R[rb];
    return 0;
}

/* 移位立即数（Imm 为 32 位） */
/*
 * @brief  操作码 SHLI：立即数左移 Rd = Ra << Imm(32)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_SHLI Rd(1B) Ra(1B) Imm(4B,大端)
 */
static uint8_t vmh_shli(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  ra  = vm_rd_u8(vm);
    uint32_t imm = vm_rd_u32(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] << imm;
    return 0;
}

/*
 * @brief  操作码 SHRI：立即数右移 Rd = Ra >> Imm(32)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_SHRI Rd(1B) Ra(1B) Imm(4B,大端)
 */
static uint8_t vmh_shri(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  rd  = vm_rd_u8(vm);
    uint8_t  ra  = vm_rd_u8(vm);
    uint32_t imm = vm_rd_u32(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] >> imm;
    return 0;
}

/*
 * @brief  操作码 NOT：按位取反 Rd = ~Ra
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_NOT Rd(1B) Ra(1B)
 */
static uint8_t vmh_not(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), rs = vm_rd_u8(vm);
    vm->cpu.R[rd] = ~vm->cpu.R[rs];
    return 0;
}

/*
 * @brief  操作码 AND：按位与 Rd = Ra & Rb
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_AND Rd(1B) Ra(1B) Rb(1B)
 */
static uint8_t vmh_and(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] & vm->cpu.R[rb];
    return 0;
}

/*
 * @brief  操作码 OR：按位或 Rd = Ra | Rb
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_OR Rd(1B) Ra(1B) Rb(1B)
 */
static uint8_t vmh_or(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] | vm->cpu.R[rb];
    return 0;
}

/*
 * @brief  操作码 XOR：按位异或 Rd = Ra ^ Rb
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_XOR Rd(1B) Ra(1B) Rb(1B)
 */
static uint8_t vmh_xor(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rd = vm_rd_u8(vm), ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    vm->cpu.R[rd] = vm->cpu.R[ra] ^ vm->cpu.R[rb];
    return 0;
}

/* ===================== 跳转 ===================== */
/* 绝对跳转：PC = R[Rb] */
/*
 * @brief  操作码 B：绝对跳转 PC = R[Rb]
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_B Rb(1B)
 */
static uint8_t vmh_b(vm_t* vm)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint8_t rb = vm_rd_u8(vm);
    vm->cpu.R[VM_PC] = vm->cpu.R[rb];
    return 0;
}

/* 相对+（寄存器）：PC = (操作数地址) + R[Rb] = (PC-1) + R[Rb] */
/*
 * @brief  操作码 BR：相对跳转（寄存器）PC = (操作数地址) + R[Rb]
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_BR Rb(1B)；操作数地址 = PC-1（已跳过操作码）
 */
static uint8_t vmh_br(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rb = vm_rd_u8(vm);
    vm->cpu.R[VM_PC] = (vm->cpu.R[VM_PC] - 1) + vm->cpu.R[rb];
    return 0;
}

/* 相对-（寄存器）：PC = (操作数地址) - R[Rb] = (PC-1) - R[Rb] */
/*
 * @brief  操作码 BL：相对跳转（左/减，寄存器）PC = (操作数地址) - R[Rb]
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_BL Rb(1B)
 */
static uint8_t vmh_bl(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rb = vm_rd_u8(vm);
    vm->cpu.R[VM_PC] = (vm->cpu.R[VM_PC] - 1) - vm->cpu.R[rb];
    return 0;
}

/* 绝对跳转（立即数）：PC = Imm */
/*
 * @brief  操作码 BI：绝对跳转（立即数）PC = Imm(32)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_BI Imm(4B,大端)
 */
static uint8_t vmh_bi(vm_t* vm)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint32_t imm = vm_rd_u32(vm);
    vm->cpu.R[VM_PC] = imm;
    return 0;
}

/* 相对+（立即数）：PC = 指令末地址 + Imm */
/*
 * @brief  操作码 BIR：相对跳转（立即数）PC += Imm(32)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_BIR Imm(4B,大端)
 */
static uint8_t vmh_bir(vm_t* vm)
{
    vm_rd_u8(vm);
    uint32_t imm = vm_rd_u32(vm);
    vm->cpu.R[VM_PC] += imm;
    return 0;
}

/* 相对-（立即数）：PC = 指令末地址 - Imm，越界报错 */
/*
 * @brief  操作码 BIL：相对跳转（左/减，立即数）PC -= Imm(32)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（向下越界返回 VM_BIL_PCZERO_ERROR）
 * @note   汇编结构：VM_BIL Imm(4B,大端)
 */
static uint8_t vmh_bil(vm_t* vm)
{
    vm_rd_u8(vm);
    uint32_t imm = vm_rd_u32(vm);
    if (vm->cpu.R[VM_PC] < imm) return VM_BIL_PCZERO_ERROR;
    vm->cpu.R[VM_PC] -= imm;
    return 0;
}

/* 条件跳转通用：cond 为真则 PC=Imm，否则已自然越过整条指令 */
/*
 * @brief  条件跳转通用内核：cond 为真则 PC = Imm(32)，否则自然落到下一条
 * @param  vm_t* vm      当前虚拟机实例
 * @param  uint8_t cond  跳转条件（由调用方根据 CMP_FLAG 计算：EQ/GT/LT 组合）
 * @return static uint8_t 错误码（恒为 0）
 * @note   所有 BEQ/BNE/BGT/BGE/BLT/BLE 立即数条件跳转均经此函数；
 *          Imm 取完后 PC 已指向下一条指令，故 cond 假时无需额外处理。
 */
static uint8_t vmh_cond_jump(vm_t* vm, uint8_t cond)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint32_t imm = vm_rd_u32(vm);          /* 取完立即数后 PC 已指向下一条指令 */
    if (cond) vm->cpu.R[VM_PC] = imm;
    return 0;
}

static uint8_t vmh_beqi(vm_t* vm) { return vmh_cond_jump(vm, vm->cpu.R[VM_CMP_FLAG] == VM_CMP_EQ); }
static uint8_t vmh_bnei(vm_t* vm) { return vmh_cond_jump(vm, vm->cpu.R[VM_CMP_FLAG] != VM_CMP_EQ); }
/*
 * @brief  操作码 BGTI：大于跳转 PC = Imm（CMP_FLAG == GT）
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_BGTI Imm(4B)；派发表中 VM_BGTI 映射到本函数 vmh_bglt。
 */
static uint8_t vmh_bglt(vm_t* vm) { return vmh_cond_jump(vm, vm->cpu.R[VM_CMP_FLAG] == VM_CMP_GT); }
/*
 * @brief  操作码 BGEI：大于等于跳转 PC = Imm（CMP_FLAG == GT 或 EQ）
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_BGEI Imm(4B)
 */
static uint8_t vmh_bgei(vm_t* vm) { return vmh_cond_jump(vm, vm->cpu.R[VM_CMP_FLAG] == VM_CMP_GT
                                                                  || vm->cpu.R[VM_CMP_FLAG] == VM_CMP_EQ); }
/*
 * @brief  操作码 BLTI：小于跳转 PC = Imm（CMP_FLAG == LT）
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_BLTI Imm(4B)
 */
static uint8_t vmh_bltr(vm_t* vm) { return vmh_cond_jump(vm, vm->cpu.R[VM_CMP_FLAG] == VM_CMP_LT); }
/*
 * @brief  操作码 BLEI：小于等于跳转 PC = Imm（CMP_FLAG == LT 或 EQ）
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_BLEI Imm(4B)
 */
static uint8_t vmh_blei(vm_t* vm) { return vmh_cond_jump(vm, vm->cpu.R[VM_CMP_FLAG] == VM_CMP_LT
                                                                  || vm->cpu.R[VM_CMP_FLAG] == VM_CMP_EQ); }

/* ===================== 比较 ===================== */
/*
 * @brief  比较 a/b 并设置比较标志（内部辅助，被各 CMP 处理器调用）
 * @param  uint32_t a     左操作数
 * @param  uint32_t b     右操作数
 * @param  uint32_t* flag 输出：VM_CMP_EQ / VM_CMP_GT / VM_CMP_LT
 * @return 无（void），结果写入 VM_CMP_FLAG
 * @note   结果写入 VM_CMP_FLAG 寄存器，供后续条件跳转指令判断。
 */
static void vm_set_cmp_flag(uint32_t a, uint32_t b, uint32_t* flag)
{
    /* 整数比较按有符号 32 位解释（C 类语言语义：-1 < 0） */
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sa == sb)      *flag = VM_CMP_EQ;
    else if (sa > sb)  *flag = VM_CMP_GT;
    else               *flag = VM_CMP_LT;
}

/*
 * @brief  操作码 CMP：寄存器比较 Ra,Rb，设置 CMP_FLAG
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_CMP Ra(1B) Rb(1B)；CMP_FLAG = (Ra==Rb?EQ:Ra>Rb?GT:LT)
 */
static uint8_t vmh_cmp(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    vm_set_cmp_flag(vm->cpu.R[ra], vm->cpu.R[rb], &vm->cpu.R[VM_CMP_FLAG]);
    return 0;
}

/*
 * @brief  操作码 CMP8：8 位立即数比较 Ra 与 Imm(8)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_CMP8 Ra(1B) Imm(1B)
 */
static uint8_t vmh_cmp8(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  ra  = vm_rd_u8(vm);
    uint8_t  imm = vm_rd_u8(vm);
    vm_set_cmp_flag(vm->cpu.R[ra], (uint32_t)imm, &vm->cpu.R[VM_CMP_FLAG]);
    return 0;
}

/*
 * @brief  操作码 CMPF：浮点比较 Rs,Rt（位级 float 转换）
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_CMPF Rs(1B) Rt(1B)
 */
static uint8_t vmh_cmpf(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t ra = vm_rd_u8(vm), rb = vm_rd_u8(vm);
    float a = pal_u32_to_f32(vm->cpu.R[ra]);
    float b = pal_u32_to_f32(vm->cpu.R[rb]);
    if (a == b)            vm->cpu.R[VM_CMP_FLAG] = VM_CMP_EQ;
    else if (a > b)        vm->cpu.R[VM_CMP_FLAG] = VM_CMP_GT;
    else                   vm->cpu.R[VM_CMP_FLAG] = VM_CMP_LT;
    return 0;
}

/*
 * @brief  操作码 CMPIF：浮点立即数比较 Rs 与 Imm(32)
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_CMPIF Rs(1B) Imm(4B)
 */
static uint8_t vmh_cmpif(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  ra  = vm_rd_u8(vm);
    uint32_t imm = vm_rd_u32(vm);
    float a = pal_u32_to_f32(vm->cpu.R[ra]);
    float b = pal_u32_to_f32(imm);
    if (a == b)            vm->cpu.R[VM_CMP_FLAG] = VM_CMP_EQ;
    else if (a > b)        vm->cpu.R[VM_CMP_FLAG] = VM_CMP_GT;
    else                   vm->cpu.R[VM_CMP_FLAG] = VM_CMP_LT;
    return 0;
}

/* ===================== 出入栈 ===================== */
/*
 * @brief  操作码 PUSH：将寄存器 Rd 压入栈区
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（栈溢出返回 VM_STACK_OVERFLOW_ERROR）
 * @note   汇编结构：VM_PUSH Rd(1B)；底层调用通用栈辅助 vm_push。
 */
static uint8_t vmh_push(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  rd = vm_rd_u8(vm);
    return vm_push(vm, vm->cpu.R[rd]);
}

/*
 * @brief  操作码 POP：从栈区弹出一个字到寄存器 Rd
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（栈下溢返回 VM_STACK_UNDERFLOW_ERROR）
 * @note   汇编结构：VM_POP Rd(1B)
 */
static uint8_t vmh_pop(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t  rd  = vm_rd_u8(vm);
    uint32_t val = 0;
    uint8_t err  = vm_pop(vm, &val);
    if (err) return err;
    vm->cpu.R[rd] = val;
    return 0;
}

/* ===================== 函数调用（堆帧） =====================
 * CALL Rb,Imm：在堆中分配帧（返回地址4 + 保存FP4 + 局部Imm），
 *   把「帧指针（堆句柄）」压入栈区（栈只存这一个指针），FP 指向帧基址，PC=目标。
 * RET：从 FP 帧读出返回地址与 saved FP，释放堆帧（确定性的 LIFO 释放），
 *   弹栈恢复帧指针，PC=返回地址。 */
/*
 * @brief  操作码 CALL：函数调用（正常调用栈）
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（栈溢出返回 VM_STACK_OVERFLOW_ERROR）
 * @note   汇编结构：VM_CALL Rb(1B) Imm(4B)
 *          - 帧直接铺在调用栈上：[FP+0]=返回地址，[FP+4]=保存的FP，[FP+8..]=参数/局部/溢出区
 *            FP 改为栈内字节偏移（原语义为堆句柄），PC = R[Rb]（目标地址）。
 *            零堆分配、零句柄表、零位图扫描，纯 O(1) bump 指针。
 */
static uint8_t vmh_call(vm_t* vm)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint8_t  rb = vm_rd_u8(vm);
    uint32_t frame_sz = vm_rd_u32(vm);     /* 参数+局部+溢出区字节数(=编译器 frame_imm) */
    uint32_t ret_addr = vm->cpu.R[VM_PC];  /* 当前 PC 已是下一条指令 = 返回地址 */
    uint32_t sp = vm->cpu.R[VM_SP];
    uint32_t new_sp = sp + 8u + frame_sz;  /* 帧布局：[ret 4][saved_fp 4][frame_sz] */
    if (new_sp > vm->stack.size) return VM_STACK_OVERFLOW_ERROR;
    uint8_t* b = vm->stack.base;
    *(uint32_t*)(b + sp + 0) = ret_addr;          /* [FP+0]  = 返回地址 */
    *(uint32_t*)(b + sp + 4) = vm->cpu.R[VM_FP];  /* [FP+4]  = 保存的 FP */
    vm->cpu.R[VM_FP] = sp;                        /* FP = 帧基址(栈内字节偏移) */
    vm->cpu.R[VM_SP] = new_sp;
    vm->cpu.R[VM_PC] = vm->cpu.R[rb];             /* 跳转目标 */
    return 0;
}

/*
 * @brief  操作码 RET：函数返回（回收调用栈帧）
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   FP 指向帧基址(栈内偏移)：读出 [FP+0] 返回地址与 [FP+4] 保存的 FP，
 *          SP 直接回退到 FP（整帧一次性释放），FP = 保存的 FP，PC = 返回地址。
 */
static uint8_t vmh_ret(vm_t* vm)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint32_t fp = vm->cpu.R[VM_FP];
    /* 伪造的 FP（没有对应 CALL 的 RET，或栈帧被破坏）会让读取
     * 跑到栈窗口之外，故解引用前先校验。 */
    if (fp > vm->stack.size || vm->stack.size - fp < 8u)
        return VM_STACK_UNDERFLOW_ERROR;
    uint8_t* b = vm->stack.base;
    uint32_t ret_addr = *(uint32_t*)(b + fp + 0);
    uint32_t saved_fp = *(uint32_t*)(b + fp + 4);
    if (ret_addr >= vm->prog.used) return VM_BIL_PCZERO_ERROR;
    vm->cpu.R[VM_SP] = fp;                 /* 释放整帧（bump 回退） */
    vm->cpu.R[VM_FP] = saved_fp;
    vm->cpu.R[VM_PC] = ret_addr;
    return 0;
}

/* 动态内存分配/释放（依赖 VM_MM：type 决定池/变长区，运行期无特判） */
/*
 * @brief  操作码 ALLOC：动态分配堆块，句柄写入 Rd
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0；失败则 Rd 写为 0）
 * @note   汇编结构：VM_ALLOC Rd(1B) Rtype(1B) Imm(4B)
 *          - Rtype: 1/2/4=固定池（按尺寸），0=变长区；经 vm_mm_alloc 分配。
 */
static uint8_t vmh_alloc(vm_t* vm)
{
    vm_rd_u8(vm);                          /* 跳过操作码 */
    uint8_t  rd   = vm_rd_u8(vm);
    uint8_t  type = vm_rd_u8(vm);          /* 编译期 type：1/2/4=池，0=变长区 */
    uint32_t n    = vm_rd_u32(vm);
    uint32_t hand = vm_mm_alloc(vm, type, n);
    vm->cpu.R[rd] = hand;
    if (hand == 0) {
        /* 仅变长区耗尽可经调度器扩容后重试；池/句柄表耗尽返回致命错 */
        return (vm_mm_last_err(vm) == VM_MM_ERR_VAR_FULL) ? VM_OOM_ERROR : VM_DATA_OVERFLOW_ERROR;
    }
    return 0;
}

/*
 * @brief  操作码 FREE：释放 Rs 指向的堆块
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_FREE Rs(1B)；经 vm_mm_free 释放。
 */
static uint8_t vmh_free(vm_t* vm)
{
    vm_rd_u8(vm);
    uint8_t rs = vm_rd_u8(vm);
    vm_mm_free(vm, vm->cpu.R[rs]);
    return 0;
}

/* 系统调用：把编号分发给 PAL 默认实现（pal_syscall） */
/*
 * @brief  操作码 SYSCALL：触发宿主系统调用
 * @param  vm_t* vm  当前虚拟机实例
 * @return uint8_t 错误码（恒为 0）
 * @note   汇编结构：VM_SYSCALL Imm(4B)；编号交给 PAL 的 pal_syscall 默认实现；
 *          未注册回调时为空操作。
 */
static uint8_t vmh_syscall(vm_t* vm)
{
    vm_rd_u8(vm);
    uint32_t nr = vm_rd_u32(vm);
    pal_syscall(vm, nr);   /* PAL 默认实现；无需上层回调 */
    return 0;
}

/* HALT：操作码 0 真正终止（不推进 PC，返回 VM_HALT，使 0 成为严格停机指令） */
/*
 * @brief  停机（操作码 0）：返回 VM_HALT 使解释器严格停机
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 VM_HALT）
 * @note   不推进 PC；vm_run 收到 VM_HALT 视为正常终止（非错误）。
 */
static uint8_t vmh_halt(vm_t* vm)
{
    (void)vm;
    return VM_HALT;
}

/* 未定义操作码（保留码 16 / 越界码）：单字节空操作，推进 PC 避免死循环 */
/*
 * @brief  未定义/保留操作码回落：单字节空操作
 * @param  vm_t* vm  当前虚拟机实例
 * @return static uint8_t 错误码（恒为 0）
 * @note   仅推进 PC 避免死循环；保留码 16 等越界码均回落到此。
 */
static uint8_t vmh_nop(vm_t* vm)
{
    vm->cpu.R[VM_PC]++;
    return 0;
}

/* ===================== 派发表 =====================
 * s_dispatch[op] = 操作码 op 对应的处理函数（vmh_*）。
 * vm_step 取出的首字节即为 op，以其为下标在此查表得到 handler 并调用；
 * 下标越界（op >= VM_DISPATCH_SIZE）或表项为 NULL 时回落到 vmh_nop，
 * 保证未知 / 保留操作码不会让解释器死循环。
 * 初始化顺序：本数组在文件末尾定义，其引用的 vmh_* 均为上方 static 函数，
 * 由编译器在编译期完成地址绑定（C 允许后向引用 static 函数）。 */
static const vm_op_handler s_dispatch[VM_DISPATCH_SIZE] = {
    [0]        = vmh_halt,  /* HALT（操作码0）：返回 VM_HALT，严格停机 */
    [VM_MOV]   = vmh_mov,
    [VM_MOV8]  = vmh_mov8,
    [VM_MOV16] = vmh_mov16,
    [VM_MOV32] = vmh_mov32,
    [VM_LDR]   = vmh_ldr,
    [VM_STR]   = vmh_str,
    [VM_LDRX]  = vmh_ldrx,
    [VM_STRX]  = vmh_strx,
    [VM_LDRXB] = vmh_ldrxb,
    [VM_STRXB] = vmh_strxb,
    [VM_LDRXH] = vmh_ldrxh,
    [VM_STRXH] = vmh_strxh,
    [VM_I2F]   = vmh_i2f,
    [VM_F2I]   = vmh_f2i,
    [VM_LDRH]  = vmh_ldrh,
    [VM_STRH]  = vmh_strh,
    [VM_ADD]   = vmh_add,
    [VM_ADDI]  = vmh_addi,
    [VM_SUB]   = vmh_sub,
    [VM_SUBI]  = vmh_subi,
    [VM_MUL]   = vmh_mul,
    [VM_MULF]  = vmh_mulf,
    [VM_DIV]   = vmh_div,
    [VM_DIVF]  = vmh_divf,
    [VM_MOD]   = vmh_mod,
    [VM_SHL]   = vmh_shl,
    [VM_SHR]   = vmh_shr,
    [VM_NOT]   = vmh_not,
    [VM_AND]   = vmh_and,
    [VM_OR]    = vmh_or,
    [VM_XOR]   = vmh_xor,
    [VM_ADDF]  = vmh_addf,
    [VM_SUBF]  = vmh_subf,
    [VM_SHLI]  = vmh_shli,
    [VM_SHRI]  = vmh_shri,
    [VM_B]     = vmh_b,
    [VM_BR]    = vmh_br,
    [VM_BL]    = vmh_bl,
    [VM_BI]    = vmh_bi,
    [VM_BIR]   = vmh_bir,
    [VM_BIL]   = vmh_bil,
    [VM_BEQI]  = vmh_beqi,
    [VM_BNEI]  = vmh_bnei,
    [VM_BGTI]  = vmh_bglt,
    [VM_BGEI]  = vmh_bgei,
    [VM_BLTI]  = vmh_bltr,
    [VM_BLEI]  = vmh_blei,
    [VM_CMP]   = vmh_cmp,
    [VM_CMP8]  = vmh_cmp8,
    [VM_CMPF]  = vmh_cmpf,
    [VM_CMPIF] = vmh_cmpif,
    [VM_PUSH]  = vmh_push,
    [VM_POP]   = vmh_pop,
    [VM_CALL]  = vmh_call,
    [VM_RET]   = vmh_ret,
    [VM_ALLOC] = vmh_alloc,
    [VM_FREE]  = vmh_free,
    [VM_SYSCALL] = vmh_syscall,
    /* 注：操作码 16 在指令集中保留未用，回落到 vmh_nop */
};

/* ===================== 公共 API ===================== */

/*
 * @brief  虚拟机初始化（对外 API）
 * @param  vm_t* vm  当前虚拟机实例（内存窗口须由 loader 先申请填入）
 * @return 无（void）
 * @note   仅做 CPU/堆运行时初始化（清零 used、初始化 MM）；
 *          VM 不分配内存、不假定 task_id，从而与内存管理解耦。
 */
void vm_init(vm_t* vm)
{
    /* 仅做 CPU 运行时初始化。内存窗口(prog/data/stack)与 VM_MM 堆布局必须由
     * 调用方负责：先向 MM 申请窗口填入本结构，再调用 vm_mm_init（默认池）
     * 或 vm_mm_init_layout（按镜像头）。
     * 这里绝不能自己调 vm_mm_init —— 它会按默认池尺寸去布置一个按镜像头
     * 精确计算的小窗口，导致 mark_range 的 len 下溢并越界写位图。 */
    pal_memset(&vm->cpu.R, 0, sizeof(vm->cpu.R));   /* 清零寄存器：SP/FP 归零，保证每次运行栈帧链从干净状态开始 */
    vm->prog.used  = 0;
    vm->data.used  = 0;
    vm->stack.used = 0;
    vm->stack_peak = 0;
    vm->step_count = 0;
    vm->profiling  = 0;
    vm->profile_budget = 0;

    DBG(DEBUG_COMP_VM, DEBUG_LEVEL_INFO,
        "init: GPR=%u code_win=%u B data_win=%u B stack_win=%u B (caller inits MM)\n",
        (unsigned)VM_R_SIZE, vm->prog.size, vm->data.size, vm->stack.size);
}

/*
 * @brief  复位虚拟机（对外 API）
 * @param  vm_t* vm  当前虚拟机实例
 * @return 无（void）
 * @note   清零寄存器文件与 prog/data/stack 全部内存，并重建 MM。
 */
void vm_reset(vm_t* vm)
{
    pal_memset(&vm->cpu.R, 0, sizeof(vm->cpu.R));
    pal_memset(vm->prog.base,  0, vm->prog.size);
    pal_memset(vm->stack.base, 0, vm->stack.size);
    /* data 区不在这里直接 memset：那样会先把 MM header 清掉，之后就无法
     * 得知原来的池大小。交给 vm_mm_reset —— 它按现有 header 记录的布局
     * 整片清零并重建 header / 位图 / 池基址寄存器。 */
    vm_mm_reset(vm);

    vm->prog.used  = 0;
    vm->data.used  = 0;
    vm->stack.used = 0;
    /* profiling 状态也要一起复位，否则复用同一个 VM 时
     * 下一次运行会立刻返回 VM_PROFILE_DONE。 */
    vm->step_count     = 0;
    vm->stack_peak     = 0;
    vm->profiling      = 0;
    vm->profile_budget = 0;

    DBG(DEBUG_COMP_VM, DEBUG_LEVEL_DEBUG,
        "reset: cleared regs/prog/data/stack; MM rebuilt (code_win=%u data_win=%u stack_win=%u)\n",
        vm->prog.size, vm->data.size, vm->stack.size);
}

/*
 * @brief  载入程序（对外 API）
 * @param  vm_t* vm           当前虚拟机实例
 * @param  const uint8_t* code 程序字节流
 * @param  uint32_t len        字节流长度
 * @return 无（void）
 * @note   拷贝到 prog.base（超出容量截断），更新 used，PC 归零。
 */
void vm_load(vm_t* vm, const uint8_t* code, uint32_t len)
{
    uint32_t n = (len < vm->prog.size) ? len : vm->prog.size;
    pal_memcpy(vm->prog.base, code, n);
    vm->prog.used = n;
    vm->cpu.R[VM_PC] = 0;
}

/*
 * @brief  执行单条指令（引擎派发入口）
 * @param  vm_t* vm  当前虚拟机实例
 * @return uint8_t   错误码：0=成功，VM_HALT=正常停机，其余为错误码
 * @note   取 PC 处操作码 → 在 s_dispatch 查 handler → 调用之。
 *          handler 内部先 vm_rd_u8 跳过操作码再读操作数；跳转类直接改写 PC。
 *          下标越界或表项为 NULL 时回落 vmh_nop，避免死循环。
 */
/* 基线实现（旧派发）：vm_step 包装 + s_dispatch 函数指针间接调用 */
uint8_t vm_step(vm_t* vm)
{
    uint8_t op = vm->prog.base[vm->cpu.R[VM_PC]];
    vm_op_handler h = (op < VM_DISPATCH_SIZE) ? s_dispatch[op] : NULL;
    if (h == NULL) h = vmh_nop;
    DBG(DEBUG_COMP_VM, DEBUG_LEVEL_TRACE, "dispatch: PC=%u op=%u SP=%u FP=%u\n",
        vm->cpu.R[VM_PC], op, vm->cpu.R[VM_SP], vm->cpu.R[VM_FP]);
    uint8_t err = h(vm);
    if (vm->cpu.R[VM_ZR] != 0) vm->cpu.R[VM_ZR] = 0;
    if (err != 0 && err != VM_HALT) {
        /* 错误：打出语义名 + 出错现场寄存器，便于定位是哪条指令、栈/帧处于什么状态。
         * 注意：这里仅观测，不改变控制流（控制流仍由 vm_run 依据 err 决定停机/OOM 重试）。 */
        DBG(DEBUG_COMP_VM, DEBUG_LEVEL_ERROR,
            "FAULT op=%u PC=%u code=%s(%u) SP=%u FP=%u\n",
            op, vm->cpu.R[VM_PC], vm_err_name(err), err, vm->cpu.R[VM_SP], vm->cpu.R[VM_FP]);
    }
    return err;
}

uint8_t vm_run(vm_t* vm)
{
    DBG(DEBUG_COMP_VM, DEBUG_LEVEL_INFO,
        "run: start PC=%u code_len=%u max_steps=%u (threaded=%d)\n",
        vm->cpu.R[VM_PC], vm->prog.used, (unsigned)VM_RUN_MAX_STEPS,
        (int)VM_THREADED);
#if defined(__GNUC__) && !defined(VM_NO_THREADED_DISPATCH)
    /* ---- Threaded dispatch（computed goto，GCC/Clang）：消除 vm_step 间接函数调用，
     *      每条指令执行后直接跳回 dispatch；分支直接、可整体寄存器分配，典型提速 1.5~3x。
     *      各 handler 仍为原 vmh_* 函数（直接调用，编译器可内联）；单步 API 仍走 vm_step。---- */
    static const void* const dtable[64] = {
        [0 ... 63] = &&L_NOP,
        [0]             = &&L_HALT,
        [VM_MOV]   = &&L_MOV,    [VM_MOV8]  = &&L_MOV8,   [VM_MOV16] = &&L_MOV16,
        [VM_MOV32] = &&L_MOV32,  [VM_LDR]   = &&L_LDR,    [VM_STR]   = &&L_STR,
        [VM_LDRX]  = &&L_LDRX,   [VM_STRX]  = &&L_STRX,   [VM_LDRXB] = &&L_LDRXB,
        [VM_STRXB] = &&L_STRXB,  [VM_LDRXH] = &&L_LDRXH,  [VM_STRXH] = &&L_STRXH,
        [VM_I2F]   = &&L_I2F,    [VM_F2I]   = &&L_F2I,    [VM_LDRH]  = &&L_LDRH,
        [VM_STRH]  = &&L_STRH,   [VM_ADD]   = &&L_ADD,    [VM_ADDI]  = &&L_ADDI,
        [VM_SUB]   = &&L_SUB,    [VM_SUBI]  = &&L_SUBI,   [VM_MUL]   = &&L_MUL,
        [VM_MULF]  = &&L_MULF,   [VM_DIV]   = &&L_DIV,    [VM_DIVF]  = &&L_DIVF,
        [VM_MOD]   = &&L_MOD,    [VM_SHL]   = &&L_SHL,    [VM_SHR]   = &&L_SHR,
        [VM_NOT]   = &&L_NOT,    [VM_AND]   = &&L_AND,    [VM_OR]    = &&L_OR,
        [VM_XOR]   = &&L_XOR,    [VM_ADDF]  = &&L_ADDF,   [VM_SUBF]  = &&L_SUBF,
        [VM_SHLI]  = &&L_SHLI,   [VM_SHRI]  = &&L_SHRI,   [VM_B]     = &&L_B,
        [VM_BR]    = &&L_BR,     [VM_BL]    = &&L_BL,     [VM_BI]    = &&L_BI,
        [VM_BIR]   = &&L_BIR,    [VM_BIL]   = &&L_BIL,    [VM_BEQI]  = &&L_BEQI,
        [VM_BNEI]  = &&L_BNEI,   [VM_BGTI]  = &&L_BGTI,   [VM_BGEI]  = &&L_BGEI,
        [VM_BLTI]  = &&L_BLTI,   [VM_BLEI]  = &&L_BLEI,   [VM_CMP]   = &&L_CMP,
        [VM_CMP8]  = &&L_CMP8,   [VM_CMPF]  = &&L_CMPF,   [VM_CMPIF] = &&L_CMPIF,
        [VM_PUSH]  = &&L_PUSH,   [VM_POP]   = &&L_POP,    [VM_CALL]  = &&L_CALL,
        [VM_RET]   = &&L_RET,    [VM_ALLOC] = &&L_ALLOC,  [VM_FREE]  = &&L_FREE,
        [VM_SYSCALL] = &&L_SYSCALL,
    };
    uint8_t  err = 0;
    uint32_t nsteps = 0;
    uint32_t err_pc = 0;       /* OOM 重试：回退到本条指令起始 PC */
    goto dispatch;

dispatch:
    /* 预算耗尽：程序并未跑完。这里若直接返回 0，超时便与正常停机
     * 无法区分（曾出现 fib(38) 跑 641 ms 却被判成功、且完全没有输出）。 */
    if (nsteps++ > VM_RUN_MAX_STEPS) return VM_STEP_LIMIT_ERROR;   /* 防 host 侧死循环（同原 steps 上限） */
    if (vm->profiling) {
        if (vm->profile_budget && vm->step_count >= vm->profile_budget) return VM_PROFILE_DONE;
        if (vm->cpu.R[VM_SP] > vm->stack_peak) vm->stack_peak = vm->cpu.R[VM_SP];
        vm->step_count++;
    }
    {
        uint32_t pc = vm->cpu.R[VM_PC];
        if (pc >= vm->prog.used) return VM_PC_OUT_OF_RANGE_ERROR;
        uint8_t op = vm->prog.base[pc];
        if (op == 0) return 0;                   /* HALT（操作码 0）严格停机 */
        err_pc = pc;
        goto *dtable[op];
    }

    L_HALT:    return 0;
    L_MOV:     if ((err = vmh_mov(vm))   != 0) goto run_err; goto dispatch;
    L_MOV8:    if ((err = vmh_mov8(vm))  != 0) goto run_err; goto dispatch;
    L_MOV16:   if ((err = vmh_mov16(vm)) != 0) goto run_err; goto dispatch;
    L_MOV32:   if ((err = vmh_mov32(vm)) != 0) goto run_err; goto dispatch;
    L_LDR:     if ((err = vmh_ldr(vm))   != 0) goto run_err; goto dispatch;
    L_STR:     if ((err = vmh_str(vm))   != 0) goto run_err; goto dispatch;
    L_LDRX:    if ((err = vmh_ldrx(vm))  != 0) goto run_err; goto dispatch;
    L_STRX:    if ((err = vmh_strx(vm))  != 0) goto run_err; goto dispatch;
    L_LDRXB:   if ((err = vmh_ldrxb(vm)) != 0) goto run_err; goto dispatch;
    L_STRXB:   if ((err = vmh_strxb(vm)) != 0) goto run_err; goto dispatch;
    L_LDRXH:   if ((err = vmh_ldrxh(vm)) != 0) goto run_err; goto dispatch;
    L_STRXH:   if ((err = vmh_strxh(vm)) != 0) goto run_err; goto dispatch;
    L_I2F:     if ((err = vmh_i2f(vm))   != 0) goto run_err; goto dispatch;
    L_F2I:     if ((err = vmh_f2i(vm))   != 0) goto run_err; goto dispatch;
    L_LDRH:    if ((err = vmh_ldrh(vm))  != 0) goto run_err; goto dispatch;
    L_STRH:    if ((err = vmh_strh(vm))  != 0) goto run_err; goto dispatch;
    L_ADD:     if ((err = vmh_add(vm))   != 0) goto run_err; goto dispatch;
    L_ADDI:    if ((err = vmh_addi(vm))  != 0) goto run_err; goto dispatch;
    L_SUB:     if ((err = vmh_sub(vm))   != 0) goto run_err; goto dispatch;
    L_SUBI:    if ((err = vmh_subi(vm))  != 0) goto run_err; goto dispatch;
    L_MUL:     if ((err = vmh_mul(vm))   != 0) goto run_err; goto dispatch;
    L_MULF:    if ((err = vmh_mulf(vm))  != 0) goto run_err; goto dispatch;
    L_DIV:     if ((err = vmh_div(vm))   != 0) goto run_err; goto dispatch;
    L_DIVF:    if ((err = vmh_divf(vm))  != 0) goto run_err; goto dispatch;
    L_MOD:     if ((err = vmh_mod(vm))   != 0) goto run_err; goto dispatch;
    L_SHL:     if ((err = vmh_shl(vm))   != 0) goto run_err; goto dispatch;
    L_SHR:     if ((err = vmh_shr(vm))   != 0) goto run_err; goto dispatch;
    L_NOT:     if ((err = vmh_not(vm))   != 0) goto run_err; goto dispatch;
    L_AND:     if ((err = vmh_and(vm))   != 0) goto run_err; goto dispatch;
    L_OR:      if ((err = vmh_or(vm))    != 0) goto run_err; goto dispatch;
    L_XOR:     if ((err = vmh_xor(vm))   != 0) goto run_err; goto dispatch;
    L_ADDF:    if ((err = vmh_addf(vm))  != 0) goto run_err; goto dispatch;
    L_SUBF:    if ((err = vmh_subf(vm))  != 0) goto run_err; goto dispatch;
    L_SHLI:    if ((err = vmh_shli(vm))  != 0) goto run_err; goto dispatch;
    L_SHRI:    if ((err = vmh_shri(vm))  != 0) goto run_err; goto dispatch;
    L_B:       if ((err = vmh_b(vm))     != 0) goto run_err; goto dispatch;
    L_BR:      if ((err = vmh_br(vm))    != 0) goto run_err; goto dispatch;
    L_BL:      if ((err = vmh_bl(vm))    != 0) goto run_err; goto dispatch;
    L_BI:      if ((err = vmh_bi(vm))    != 0) goto run_err; goto dispatch;
    L_BIR:     if ((err = vmh_bir(vm))   != 0) goto run_err; goto dispatch;
    L_BIL:     if ((err = vmh_bil(vm))   != 0) goto run_err; goto dispatch;
    L_BEQI:    if ((err = vmh_beqi(vm))  != 0) goto run_err; goto dispatch;
    L_BNEI:    if ((err = vmh_bnei(vm))  != 0) goto run_err; goto dispatch;
    L_BGTI:    if ((err = vmh_bglt(vm))  != 0) goto run_err; goto dispatch;
    L_BGEI:    if ((err = vmh_bgei(vm))  != 0) goto run_err; goto dispatch;
    L_BLTI:    if ((err = vmh_bltr(vm))  != 0) goto run_err; goto dispatch;
    L_BLEI:    if ((err = vmh_blei(vm))  != 0) goto run_err; goto dispatch;
    L_CMP:     if ((err = vmh_cmp(vm))   != 0) goto run_err; goto dispatch;
    L_CMP8:    if ((err = vmh_cmp8(vm))  != 0) goto run_err; goto dispatch;
    L_CMPF:    if ((err = vmh_cmpf(vm))  != 0) goto run_err; goto dispatch;
    L_CMPIF:   if ((err = vmh_cmpif(vm)) != 0) goto run_err; goto dispatch;
    L_PUSH:    if ((err = vmh_push(vm))  != 0) goto run_err; goto dispatch;
    L_POP:     if ((err = vmh_pop(vm))   != 0) goto run_err; goto dispatch;
    L_CALL:    if ((err = vmh_call(vm))  != 0) goto run_err; goto dispatch;
    L_RET:     if ((err = vmh_ret(vm))   != 0) goto run_err; goto dispatch;
    L_ALLOC:   if ((err = vmh_alloc(vm)) != 0) goto run_err; goto dispatch;
    L_FREE:    if ((err = vmh_free(vm))  != 0) goto run_err; goto dispatch;
    L_SYSCALL: if ((err = vmh_syscall(vm))!= 0) goto run_err; goto dispatch;
    L_NOP:     if ((err = vmh_nop(vm))   != 0) goto run_err; goto dispatch;

run_err:
    if (err == VM_OOM_ERROR) { vm->cpu.R[VM_PC] = err_pc; return VM_OOM_ERROR; }
    /* 错误现场日志：threaded 路径直接由 handler 跳到此，不经过 vm_step，
     * 故在此补发与 vm_step 同款的 FAULT，避免"有错误码却无任何 [ERR] 输出"。
     * （非 threaded 路径由 vm_step 内的同款 DBG 负责，不会重复打印） */
    DBG(DEBUG_COMP_VM, DEBUG_LEVEL_ERROR,
        "FAULT op=%u PC=%u code=%s(%u) SP=%u FP=%u\n",
        vm->prog.base[err_pc], err_pc, vm_err_name(err), err,
        vm->cpu.R[VM_SP], vm->cpu.R[VM_FP]);
    return err;

#else
    /* 标准 C 回退（无 computed goto 的编译器）：复用 vm_step 的间接派发 */
    uint32_t steps = 0;
    while (steps < VM_RUN_MAX_STEPS) {
        if (vm->profiling && vm->profile_budget && vm->step_count >= vm->profile_budget)
            return VM_PROFILE_DONE;
        uint32_t pc = vm->cpu.R[VM_PC];
        if (pc >= vm->prog.used) break;
        if (vm->prog.base[pc] == 0) break;
        uint8_t err = vm_step(vm);
        if (vm->profiling) {
            vm->step_count++;
            if (vm->cpu.R[VM_SP] > vm->stack_peak) vm->stack_peak = vm->cpu.R[VM_SP];
        }
        if (err) {
            if (err == VM_HALT) return 0;
            if (err == VM_PROFILE_DONE) return VM_PROFILE_DONE;
            if (err == VM_OOM_ERROR) { vm->cpu.R[VM_PC] = pc; return VM_OOM_ERROR; }
            return err;
        }
        steps++;
    }
    return 0;
#endif
}

/* profiling 结束后打印 RAM 占用报表，直接回答「程序运行消耗多少 RAM / 能否塞进 F407 100KB」。
 * 注意本项目总运行内存 = CONFIG_RAM_SIZE*8（100KB），是 OS+所有任务共享的 MM 池上限；
 * prog/data/stack 三个窗口从其中划分，堆(used_peak)位于 data 窗口内、栈峰值位于 stack 窗口内。
 * 优化目标：把 data/stack 窗口收缩到 peak+margin，从而最小化实际提交内存。 */
void vm_print_ram_report(vm_t* vm)
{
    uint32_t heap_peak = 0;
    if (vm->data.size) {
        vm_mm_hdr_t* h = (vm_mm_hdr_t*)vm->data.base;
        heap_peak = h->used_peak;
    }
    uint32_t stack_peak = vm->stack_peak;
    uint32_t budget = (uint32_t)CONFIG_RAM_SIZE * 8u;   /* 项目总运行内存 100KB */
    const uint32_t margin = 4096u;                      /* 收缩窗口时的安全余量 */

    /* 当前提交：三窗口固定分配（Phase5 之前未收缩） */
    uint32_t committed_now = vm->prog.size + vm->data.size + vm->stack.size;
    /* 优化后：code 进 flash(不计 RAM)，data/stack 收到 peak + margin */
    uint32_t code_used = vm->prog.used;
    uint32_t opt_stack = stack_peak + margin;
    uint32_t opt_data  = heap_peak  + margin;
    uint32_t opt_ram   = opt_stack + opt_data;          /* F407 实际 RAM = data+stack */

    pal_printf("  [RAM report] project total budget = %u B (%u KB)   (F407: code in flash)\n",
           budget, budget / 1024u);
    pal_printf("    code   : used %u B   (window %u B)\n", code_used, vm->prog.size);
    pal_printf("    data   : heap peak %u B  (window %u B)\n", heap_peak, vm->data.size);
    pal_printf("    stack  : stack peak %u B (window %u B)\n", stack_peak, vm->stack.size);
    pal_printf("    max_frame(from image): %u B\n", vm->max_frame_size);
    pal_printf("    ---- as committed now (3 windows, before shrink) ----\n");
    pal_printf("    committed = %u B (%u KB)  %s\n", committed_now, committed_now / 1024u,
           committed_now <= budget ? "fits=YES" : "fits=NO (OVER BUDGET)");
    pal_printf("    ---- if shrunk to peak+margin(%u B) ---- (F407 RAM = data+stack)\n", margin);
    pal_printf("    opt RAM = %u B (%u KB)  %s\n", opt_ram, opt_ram / 1024u,
           opt_ram <= budget ? "fits=YES" : "fits=NO (OVER)");
}

/* 兼容旧接口：单步执行全局实例 */
/*
 * @brief  兼容旧接口：单步执行全局实例 g_vm
 * @return uint8_t   vm_step(&g_vm) 的错误码
 * @note   仅供旧代码/最小示例使用，新代码请直接调用 vm_step。
 */
uint8_t VM_RUN(void)
{
    return vm_step(&g_vm);
}
