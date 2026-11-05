/******************************************************************************
 * 文件名:   vm_image.h
 * 描述:     镜像(.osimg)布局契约——编译器(宿主侧)与 Loader(内核侧)的单一真相源。
 *           仅依赖 <stdint.h>，不 include VM.h / MM.h / Loader.h，避免内核
 *           运行时类型被宿主侧编译器拖入，也避免编译器误调到运行时函数。
 *           对应文档 compiler_design.md §8.2。
 * 版本:     0.0.1
 ******************************************************************************/
#ifndef VM_IMAGE_H_
#define VM_IMAGE_H_

#include <stdint.h>

/* 镜像魔数：'OSV1' = 0x4F535631u（防误读） */
#define VM_IMG_MAGIC  0x4F535631u

/* 运行期留给每个池的额外槽数（编译器 emit 时加在静态占用之后，
 * 供程序运行期 ALLOC 使用，使静态全局变量与动态分配互不踩踏）。
 * 这是编译器侧对"运行时余量"的保守估计，可在编译器演进时调整。 */
#define VM_IMG_POOL1_PAD  32u
#define VM_IMG_POOL2_PAD  32u
#define VM_IMG_POOL4_PAD  32u

/* 静态区布局表：编译器在 .osimg 头里写出，Loader 读它交给 vm_mm_init_layout。
 * 字段顺序即镜像头中（magic 之后）的二进制顺序，全为宿主端 u32，无填充。 */
typedef struct {
    /* —— 池总大小（含静态占用 + 运行时余量） —— */
    uint32_t pool1_cnt;          /* 1 字节池总槽数 */
    uint32_t pool2_cnt;          /* 2 字节池总槽数 */
    uint32_t pool4_cnt;          /* 4 字节池总槽数 */
    /* —— 其中被静态全局变量占用的槽数（vm_mm_init_layout 会标记这些槽已占用） —— */
    uint32_t static_pool1_used;  /* 静态占用的 1B 槽数 */
    uint32_t static_pool2_used;  /* 静态占用的 2B 槽数 */
    uint32_t static_pool4_used;  /* 静态占用的 4B 槽数 */
    /* —— 静态变长区 / 预建固定句柄（首版骨架暂为 0，预留接口） —— */
    uint32_t static_varlen_len;  /* 静态变长区字节数（大对象初值区） */
    uint32_t n_fixed_handles;    /* 由 init 预建的固定句柄数 */
    /* —— 编译期全局最大调用栈帧（含 ret4 + fp4 + 局部，即 8 + max(frame_imm)） —— */
    uint32_t max_frame_size;     /* 供 loader 据 recursion_depth 算精确栈窗口 */
} vm_static_layout_t;

/* 计算布局对应的静态数据段字节数：[1B池][2B池][4B池][静态变长区]
 * （与 vm_mm_init_layout 的池布置顺序一致，Loader 可据此 memcpy 初值）。 */
static inline uint32_t vm_img_data_len(const vm_static_layout_t* l) {
    return l->pool1_cnt * 1u + l->pool2_cnt * 2u + l->pool4_cnt * 4u
         + l->static_varlen_len;
}

#endif /* VM_IMAGE_H_ */
