/******************************************************************************
 * 文件名:   vm_isa.h
 * 描述:     VM 指令集架构（ISA）常量头——单一真相源。
 *          抽取自 VM.h，使「VM 运行时」与「宿主侧编译器」共享同一套操作码 /
 *          特殊寄存器 / 错误码 / SYSCALL 编号，避免编号漂移。
 *          本头仅依赖 config.h（取 VM_R_SIZE），不依赖任何运行时实现，
 *          因此编译器（位于 code/compiler/，在 os/ 之外）也可安全包含。
 * 版本:     0.0.1
 ******************************************************************************/
#ifndef VM_ISA_H_
#define VM_ISA_H_

#include "config.h"   /* 仅取 VM_R_SIZE，供特殊寄存器宏推导 */

/* ===================== 指令操作码 ===================== */
  //移动指令
#define VM_MOV 1             // MOV Rd,Rs;Rd=Rs;移动指令
#define VM_MOV8 2            // MOV Rd,Imm;Rd=Imm;立即数移动指令
#define VM_MOV16 3           // MOV Rd,Imm;Rd=Imm;立即数移动指令,16位
#define VM_MOV32 4           // MOV Rd,Imm;Rd=Imm;立即数移动指令,32位
#define VM_LDR 5             // LDR Rd,[Rs+Imm];Rd=内存[Rs+Imm];加载指令,立即数偏移 32位(经句柄表)
#define VM_STR 6             // STR Rd,[Rs+Imm];内存[Rs+Imm]=Rd;存储指令,立即数偏移 32位(经句柄表)
#define VM_LDRX 50            // LDRX Rd,[Rs+Imm];Rd=data.base[Rs+Imm];裸全内存加载,立即数偏移 32位(不经句柄表)
#define VM_STRX 51            // STRX Rd,[Rs+Imm];data.base[Rs+Imm]=Rd;裸全内存存储,立即数偏移 32位(不经句柄表)
#define VM_LDRXB 52           // LDRXB Rd,[Rs+Imm];Rd=符号扩展的 1 字节(经 VM_POOL1);1B池裸加载
#define VM_STRXB 53           // STRXB Rd,[Rs+Imm];data.base[Rs+Imm]=Rd低1字节(经 VM_POOL1);1B池裸存储
#define VM_LDRXH 54           // LDRXH Rd,[Rs+Imm];Rd=符号扩展的 2 字节(经 VM_POOL2);2B池裸加载
#define VM_STRXH 55           // STRXH Rd,[Rs+Imm];data.base[Rs+Imm]=Rd低2字节(经 VM_POOL2);2B池裸存储
#define VM_I2F 56             // I2F Rd,Ra;Rd=(float)Ra;int->float 转换(cast/混合运算)
#define VM_F2I 57             // F2I Rd,Ra;Rd=(int)Ra;float->int 转换(cast/混合运算)
#define VM_LDRH 58            // LDRH Rd,Rh,Ro;Rd=堆[Rh句柄+Ro偏移];寄存器偏移堆加载(4B,数组/串)
#define VM_STRH 59            // STRH Rs,Rh,Ro;堆[Rh句柄+Ro偏移]=Rs;寄存器偏移堆存储(4B,数组/串)
  //算术指令
#define VM_ADD 7             // ADD Rd,Ra,Rb;普通加法，Rd = Ra+Rb
#define VM_ADDI 8            // ADDI Rd,Ra,Imm;普通加法，Rd = Ra+Imm，立即数加法
#define VM_SUB 9             // SUB Rd,Ra,Rb;普通减法，Rd = Ra-Rb
#define VM_SUBI 10           // SUBI Rd,Ra,Imm;普通减法，Rd = Ra-Imm，立即数减法
#define VM_MUL 11            // MUL Rd,Ra,Rb;普通乘法，Rd = Ra*Rb
#define VM_MULF 12           // MULF Rd,Ra,Rb;浮点乘法，Rd = Ra*Rb
#define VM_DIV 13            // DIV Rd,Ra,Rb;普通除法，Rd = Ra/Rb
#define VM_DIVF 14           // DIVF Rd,Ra,Rb;浮点除法，Rd = Ra/Rb
#define VM_MOD 15            // MOD Rd,Ra,Rb;普通取余，Rd = Ra%Rb
#define VM_SHL 17            // SHL Rd,Ra,Rb;普通左移，Rd = Ra<<Rb
#define VM_SHR 18            // SHR Rd,Ra,Rb;普通右移，Rd = Ra>>Rb
#define VM_NOT 19            // NOT Rd,Ra;普通取反，Rd = ~Ra
#define VM_AND 20            // AND Rd,Ra,Rb;普通与操作，Rd = Ra&Rb
#define VM_OR 21             // OR Rd,Ra,Rb;普通或操作，Rd = Ra|Rb
#define VM_XOR 22            // XOR Rd,Ra,Rb;普通异或操作，Rd = Ra^Rb
 //跳转指令
#define VM_B 23             // B Rb;PC = Rb
#define VM_BR 24            // BR Rb;PC += Rb
#define VM_BL 25            // BL Rb;PC -= Rb
#define VM_BI 26            // BI Imm;PC = Imm
#define VM_BIR 27           // BIR Imm;PC += Imm
#define VM_BIL 28           // BIL Imm;PC -= Imm
#define VM_BEQI 29            // BEQI Imm;等于跳转
#define VM_BNEI 30            // BNEI Imm;不等于跳转
#define VM_BGTI 31            // BGTI Imm;大于跳转
#define VM_BGEI 32            // BGEI Imm;大于等于跳转
#define VM_BLTI 33            // BLTI Imm;小于跳转
#define VM_BLEI 34            // BLEI Imm;小于等于跳转
 //比较指令
#define VM_CMP 35            // CMP Ra,Rb;设置标志位
#define VM_CMP8 36           // CMP8 Ra,Imm;8位立即数比较
#define VM_CMPF 37           // CMPF Ra,Rb;浮点比较
#define VM_CMPIF 38          // CMPIF Ra,Imm;浮点立即数比较
 //出入栈
#define VM_PUSH 39            // PUSH Rd
#define VM_POP 40             // POP Rd
  //函数调用（堆帧）
#define VM_CALL 41            // CALL Rb,Imm;Rb=目标地址，Imm=局部变量区字节数
#define VM_RET 42             // RET;释放堆帧、恢复 FP、跳回返回地址
  //浮点算术补充
#define VM_ADDF 43            // ADDF Rd,Ra,Rb;浮点加
#define VM_SUBF 44            // SUBF Rd,Ra,Rb;浮点减
  //移位立即数
#define VM_SHLI 45            // SHLI Rd,Ra,Imm;Rd = Ra<<Imm
#define VM_SHRI 48            // SHRI Rd,Ra,Imm;Rd = Ra>>Imm
  //堆管理
#define VM_ALLOC 46           // ALLOC Rd,Rtype,Imm;Rtype=尺寸类(1/2/4=池,0=变长区)，Rd=句柄(0=失败)
#define VM_FREE 47            // FREE Rs;释放堆块
  //系统调用
#define VM_SYSCALL 49         // SYSCALL Imm;触发宿主系统调用（编号 Imm）

/* ===================== 特殊寄存器索引 ===================== */
#define VM_PC (VM_R_SIZE-1)//程序计数器
#define VM_SP (VM_R_SIZE-2)//栈指针
#define VM_FP (VM_R_SIZE-3)//帧指针
#define VM_Z (VM_R_SIZE-4)//零标志位
#define VM_N (VM_R_SIZE-5)//负标志位
#define VM_C (VM_R_SIZE-6)//进位标志位
#define VM_V (VM_R_SIZE-7)//溢出标志位
#define VM_I (VM_R_SIZE-8)//中断标志位
#define VM_CMP_FLAG (VM_R_SIZE-9)//比较标志位

/* ===================== 零寄存器 / 裸地址基址特殊寄存器 =====================
 * 保留在 GPR 顶端（VM_R_SIZE=33 时落 R19..R23），编译器寄存器分配器不得作临时寄存器：
 *   - VM_ZR    ：硬连线零寄存器（读恒为 0，写无效）
 *   - VM_DATA  ：data 区起点裸偏移（恒为 0）
 *   - VM_POOLx ：1/2/4 字节池基址裸偏移（vm_mm_init 填），供 LDRX/STRX 裸寻址
 * 注意与 VM_Z（零标志位）区分。 */
#define VM_ZR    (VM_R_SIZE-10)   // 零寄存器 R23
#define VM_DATA  (VM_R_SIZE-14)   // data 区起点裸偏移 R19
#define VM_POOL1 (VM_R_SIZE-13)   // 1 字节池基址裸偏移 R20
#define VM_POOL2 (VM_R_SIZE-12)   // 2 字节池基址裸偏移 R21
#define VM_POOL4 (VM_R_SIZE-11)   // 4 字节池基址裸偏移 R22

/* ===================== 报错码 =====================
 * 用 typedef enum 取代散落 #define：获得类型（vm_err_code_t）、调试器可读名、
 * 以及天然的错误码边界。0 统一表示“成功/无错误”；255/254 为受控停机码（非错误）。
 * 编译器（code/compiler/）与运行时共享此表，编号单一来源、不漂移。 */
typedef enum {
    VM_OK = 0,                      // 成功/无错误（与所有 return 0 一致）
    VM_ERROR = 250,                 // 未定义/通用错误（非 0，避免与“成功=0”冲突）
    VM_DIV_ZERO_ERROR = 1,          // 除0错误
    VM_MOD_ZERO_ERROR = 2,          // 取余0错误
    VM_BIL_PCZERO_ERROR = 3,        // 跳转指令PC为0错误
    VM_STACK_OVERFLOW_ERROR = 4,    // 栈溢出错误
    VM_STACK_UNDERFLOW_ERROR = 5,   // 栈下溢错误
    VM_DATA_OVERFLOW_ERROR = 6,     // 堆溢出错误
    VM_DATA_UNDERFLOW_ERROR = 7,    // 堆下溢错误
    VM_FRAME_ERROR = 8,             // 函数帧堆分配失败
    VM_OOM_ERROR = 9,              // 数据区耗尽（可扩容重试）
    VM_FATAL_ERROR = 10,           // 致命错误（扩容失败/池或句柄表耗尽）
    VM_STEP_LIMIT_ERROR = 11,      // 步数预算耗尽：程序没跑完，不能当作正常停机
    VM_PC_OUT_OF_RANGE_ERROR = 12, // PC 超出代码段（缺 HALT / 伪造跳转 / 野 RET）
    /* —— 加载器 / 宿主侧错误（镜像与清单处理，非 VM 运行时错误） ——
     * 原先这类失败一律复用 VM_FATAL_ERROR，导致"程序跑崩"与"镜像根本没加载上"
     * 无法区分；现独立成段。均非 0，故调用方 err != 0 的判断不受影响。 */
    VM_LOAD_ERR_OPEN = 20,         // 镜像 / .loading 清单文件不可读
    VM_LOAD_ERR_HEADER = 21,       // 镜像头非法：不足 48B / 魔数错 / 段长越界（截断）
    VM_LOAD_ERR_NOMEM = 22,        // 段缓冲分配失败（宿主内存不足）
    VM_LOAD_ERR_NOIMAGE = 23,      // .loading 清单缺少 image 字段
    VM_PROFILE_DONE = 254,         // profiling 步数预算到顶（受控停机，非错误）
    VM_HALT = 255                  // HALT（操作码0）正常终止，非错误
} vm_err_code_t;

/* 错误码 → 可读名。仅供 DEBUG 输出使用，绝不参与任何控制流。
 * 仅当 DEBUG 开启时编译完整映射；关闭调试时退化为单字符 stub，
 * 既保证任何模式下 VM.c 调用点可编译，又不产生映射表开销。 */
#if (DEBUG == CONFIG_ON)
static inline const char* vm_err_name(int code)
{
    switch (code) {
        case VM_OK:                  return "VM_OK";
        case VM_ERROR:               return "VM_ERROR";
        case VM_DIV_ZERO_ERROR:      return "VM_DIV_ZERO_ERROR";
        case VM_MOD_ZERO_ERROR:      return "VM_MOD_ZERO_ERROR";
        case VM_BIL_PCZERO_ERROR:    return "VM_BIL_PCZERO_ERROR";
        case VM_STACK_OVERFLOW_ERROR:   return "VM_STACK_OVERFLOW_ERROR";
        case VM_STACK_UNDERFLOW_ERROR:  return "VM_STACK_UNDERFLOW_ERROR";
        case VM_DATA_OVERFLOW_ERROR:     return "VM_DATA_OVERFLOW_ERROR";
        case VM_DATA_UNDERFLOW_ERROR:    return "VM_DATA_UNDERFLOW_ERROR";
        case VM_FRAME_ERROR:         return "VM_FRAME_ERROR";
        case VM_OOM_ERROR:           return "VM_OOM_ERROR";
        case VM_FATAL_ERROR:         return "VM_FATAL_ERROR";
        case VM_STEP_LIMIT_ERROR:    return "VM_STEP_LIMIT_ERROR";
        case VM_PC_OUT_OF_RANGE_ERROR: return "VM_PC_OUT_OF_RANGE_ERROR";
        case VM_LOAD_ERR_OPEN:       return "VM_LOAD_ERR_OPEN";
        case VM_LOAD_ERR_HEADER:     return "VM_LOAD_ERR_HEADER";
        case VM_LOAD_ERR_NOMEM:      return "VM_LOAD_ERR_NOMEM";
        case VM_LOAD_ERR_NOIMAGE:    return "VM_LOAD_ERR_NOIMAGE";
        case VM_PROFILE_DONE:        return "VM_PROFILE_DONE";
        case VM_HALT:                return "VM_HALT";
        default:                     return "UNKNOWN";
    }
}
#else
static inline const char* vm_err_name(int code) { (void)code; return "?"; }
#endif

/* ===================== 比较标志 ===================== */
#define VM_CMP_EQ 0 //等于
#define VM_CMP_GT 2 //大于
#define VM_CMP_LT 3 //小于

/* ===================== SYSCALL 编号（与 compiler_design.md §6 一致） =====================
 * 编译器（code/compiler/）与运行时宿主共用此表，编号单一来源、不漂移。 */
#define VM_SC_EXIT          1   // exit/halt
#define VM_SC_PRINT_INT     2   // R0=值
#define VM_SC_PRINT_FLOAT   3   // R0=IEEE754 位模式
#define VM_SC_PRINT_STR     4   // R0=串句柄, R1=长度
#define VM_SC_PRINT_CHAR    5   // R0=码点
#define VM_SC_PRINT_NEWLINE 6   // 换行
#define VM_SC_INPUT_INT     7   // 返回 R0
#define VM_SC_INPUT_FLOAT   8   // 返回 R0
#define VM_SC_INPUT_STR     9   // R0=缓冲句柄, R1=maxlen；返回 R0=实际长度
#define VM_SC_INPUT_CHAR    10  // 返回 R0
#define VM_SC_FLUSH         11  // 强制刷新显示

#endif /* VM_ISA_H_ */
