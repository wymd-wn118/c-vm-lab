#ifndef PROCESS_SCHEDULER_H
#define PROCESS_SCHEDULER_H

#include <stdint.h>

/* ===================== 配置 ===================== */
#define PS_MAX_PRIORITY   10   /* 优先级级数（有效取值 0 .. PS_MAX_PRIORITY-1） */
#define PS_STEP           10   /* 每个任务每轮最多执行的指令步数 */

/* 0 = 最高优先级。当前实现为严格优先级（高优先级有 READY 任务时低优先级不执行），
 * 因此平时所有任务建议用同一个默认值，才能得到时间片轮转的效果。 */
#define PS_PRIORITY_DEFAULT   0

/* ===================== 任务状态 ===================== */
#define PS_STATE_RUNNED   1    /* 运行中：已调度、尚未让出 */
#define PS_STATE_READY    2    /* 就绪：可被调度 */
#define PS_STATE_BLOCK    3    /* 阻塞：等待事件（预留） */
#define PS_STATE_DEAD     4    /* 已结束：等待 PS_Delete_Process 回收 */

/* ===================== 调度策略（预留） ===================== */
#define PS_STRATEGY_ROUND_OF_STEP   1
#define PS_STRATEGY_COLLABORATIVE   2

/* ===================== 结构体 ===================== */
typedef struct PS_Process_Thread   PS_Process_Thread;
typedef struct PS_Process_Priority PS_Process_Priority;

struct PS_Process_Thread {
    uint8_t            state;      /* PS_STATE_* */
    const char*        name;       /* 仅保存指针，调用方需保证其生命周期 */
    uint16_t           Step_Slice; /* 本轮已用步数（预留） */
    uint32_t           ID;         /* 任务 ID，同时作为 MM 的 task_id，必须 >= 3 */
    void*              body;       /* 任务体：指向 vm_t */
    PS_Process_Thread* next;
};

struct PS_Process_Priority {
    uint8_t              priority; /* 0 .. PS_MAX_PRIORITY-1 */
    PS_Process_Priority* next;
    PS_Process_Thread*   thread;   /* 该优先级下的线程链表 */
};

/* ===================== 全局 ===================== */
extern PS_Process_Priority* PS_Process_Priority_Head;

/* ===================== 任务回收回调 =====================
 * 任务从 DEAD 被回收（摘链 -> mm_reap -> 释放节点）后，通知上层做自己的清理，
 * 例如 job 层要把 task_id 槽位还回空闲位图。
 *
 * 为什么用函数指针而不是直接调用 job 的函数：
 *   调度器是下层，job 是上层。若调度器 #include "job.h" 去调 delete_Task，
 *   就成了「下层依赖上层」，而 job.c 本身又依赖 Process_Scheduler.h，
 *   两者互相指向 -> 循环依赖，且 .c 文件单独编译时会报
 *   'delete_Task' 隐式声明。
 *   改成函数指针后，调度器只保存「一个函数的地址」，并不认识 job，
 *   依赖方向仍然是 job -> Process_Scheduler，单向。
 *
 * 用法：上层调用一次 PS_SetReapCallback(my_cb) 即可；传 NULL 表示取消。 */
typedef void (*ps_reap_cb_t)(uint32_t task_id);
void PS_SetReapCallback(ps_reap_cb_t cb);

/* ===================== 接口 ===================== */

/* @brief  建立优先级链表（PS_MAX_PRIORITY 个节点）。幂等：已初始化则直接返回 0。
 * @return 0 成功；-1 MM 分配失败 */
int PS_Init(void);

/* @brief  主调度循环，死循环永不返回（所有任务结束后仍空转）。 */
int PS_Main(void);

/* @brief  调度一轮：每个 READY 任务各跑至多 PS_STEP 条指令。
 *         任务 HALT 或出错时置 PS_STATE_DEAD，交由 PS_Delete_Process 回收。
 * @return 0 成功；-1 尚未 PS_Init */
int PS_Step(void);

/* @brief  回收所有 PS_STATE_DEAD 的任务：摘链 -> mm_reap(task_id) -> mm_free(节点)。
 * @return 0 成功；-1 尚未 PS_Init */
int PS_Delete_Process(void);

/* @brief  创建一个任务并挂入对应优先级链表。
 * @param  vm      任务体（通常是 vm_t*），其生命周期由本模块在任务结束时回收
 * @param  name    任务名（只保存指针，不拷贝）
 * @param  priority 0 .. PS_MAX_PRIORITY-1
 * @param  task_id MM 任务号，必须 >= 3（0/1/2 为 MM 保留号）
 * @return 0 成功；-1 参数非法 / 未 PS_Init / MM 分配失败 */
int PS_Creat_Process_Ass(void* vm, const char* name, uint8_t priority, uint32_t task_id);

#endif /* PROCESS_SCHEDULER_H */
