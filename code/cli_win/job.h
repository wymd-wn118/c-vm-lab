#ifndef JOB_H
#define JOB_H

#include <stdint.h>

/* ===================== task_id 空间 =====================
 * MM 保留号：0 = MM_TASK_FREE，1 = MM_TASK_SYSTEM，2 = MM_TASK_COMPILER
 * 因此用户任务只能使用 3 .. MAX_TASK_NUM-1
 */
#define MAX_TASK_NUM       16
#define JOB_TASK_ID_BASE   3u
#define JOB_TASK_SLOTS     (MAX_TASK_NUM - 3)

/* ===================== 返回码 ===================== */
#define JOB_OK                          0
#define JOB_ERROR_TASK_ID_FULL          1   /* 13 个槽位全被占用 */
#define JOB_ERROR_MM_ALLOC_FAIL         2   /* 镜像/窗口分配失败 */
#define JOB_ERROR_COMPILE_FAIL          3   /* 编译失败（含语法错误） */
#define JOB_ERROR_LOAD_FAIL             4   /* 装载失败 */
#define JOB_ERROR_CREATE_PROCESS_FAIL   5   /* 注册到调度器失败 */

/* task_id 占用位图：1 = 已分配，0 = 空闲。仅在 create_Task 内部使用。 */
extern char task_id_table[JOB_TASK_SLOTS];

/* ===================== 接口 =====================
 * @brief  编译 -> 装载 -> 注册，创建一个可被调度器运行的任务。
 * @param  src             源码文本（可含多个函数）
 * @param  gc_enable       1 = 开启 VM 的 GC
 * @param  recursion_depth 栈窗口按 max_frame*depth 估算；0 = 用 loader 默认值
 * @return JOB_OK 成功；其余为 JOB_ERROR_* 错误码
 * @note   调用前需已执行 mm_init() 与 PS_Init()。
 *         成功时该任务占用的内存（vm + region）由调度器在任务结束时通过
 *         mm_reap(task_id) 统一回收，调用方无需释放。 */
uint8_t create_Task(const char* src, uint8_t gc_enable, uint32_t recursion_depth);

/* @brief  归还 task_id 槽位（把位图对应位置 0）。
 * @note   本函数会被注册为调度器的回收回调，由 Process_Scheduler 在任务被
 *         回收后调用；也可由 delete_Task 直接调用。越界 id 会被忽略。 */
void job_release_id(uint32_t task_id);

/* @brief  主动销毁任务：mm_reap(task_id) + 归还 task_id。
 * @warning 若该任务仍在调度器链表中，调用后链表会留下悬空节点。
 *          正常流程下任务 HALT 后由调度器自动回收，CLI 一般不需要调用。 */
void delete_Task(uint32_t task_id);

#endif /* JOB_H */

void delete_Task(uint32_t task_id);