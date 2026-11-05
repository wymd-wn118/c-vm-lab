#include "job.h"
#include "DEBUG.h"
#include "compiler.h"
#include "MM.h"
#include "PAL.h"
#include "Loader.h"
#include "Process_Scheduler.h"

//分配任务ID的表，一个bit对应一个任务，1代表被分配，0代表没有被分配
char task_id_table[JOB_TASK_SLOTS] = {0};




uint8_t create_Task(const char* src,uint8_t gc_enable,uint32_t recursion_depth){
    uint32_t task_id = 0;
    int required = 10230;

    /* 把「归还 task_id」的函数地址交给调度器：任务被回收时它会回调。
     * 重复注册同一个地址无副作用，所以不用额外的初始化函数。 */
    PS_SetReapCallback(job_release_id);
    //遍历所有地方，找到哦一个没有被分配的任务ID
    for (int i = 0;i<MAX_TASK_NUM-3;i++){
        if(task_id_table[i] == 0){
            task_id_table[i] = 1;
            task_id = i+3;
            break;
        }
    }
    if(task_id == 0){
        return JOB_ERROR_TASK_ID_FULL;
    }
    mm_init();
    //把输入的程序编译
    uint32_t mm_alloc_off = mm_alloc(task_id,10240);
    if(mm_alloc_off == MM_FAIL){
        task_id_table[task_id - 3] = 0;             /* 尚未分配任何块，只需归还 task_id */
        pal_printf("内存分配失败\n");
        return JOB_ERROR_MM_ALLOC_FAIL;
    }
    uint8_t* pimg = mm_ram()+mm_alloc_off;
    comp_result_t build_return = compiler_build(src,pimg,10240);
    //判断编译成功与否 
    if(build_return.ok == 0){
        if(build_return.n_errors > 0){
            mm_reap(task_id);                       /* 统一清掉本任务名下的所有块 */
            task_id_table[task_id - 3] = 0;         /* 归还 task_id，否则永不回收 */
            pal_printf("编译失败，错误行数:%d，错误信息:%s\n", build_return.err_line, build_return.err_msg);
            return JOB_ERROR_COMPILE_FAIL;
        }
        required = build_return.required;
        //把内存扩大到需要的内存量，进行重试
        mm_free(mm_alloc_off);
        mm_alloc_off = mm_alloc(task_id, required+10);
        if(mm_alloc_off == MM_FAIL){
            mm_reap(task_id);                       /* 统一清掉本任务名下的所有块 */
            task_id_table[task_id - 3] = 0;         /* 归还 task_id，否则永不回收 */
            pal_printf("内存分配失败\n");
            return JOB_ERROR_MM_ALLOC_FAIL;
        }
        pimg = mm_ram()+mm_alloc_off;               /* 必须复用外层 pimg：新分配后地址可能变了 */
        build_return = compiler_build(src,pimg,required+10);
        if(build_return.ok == 0){
            mm_reap(task_id);                       /* 统一清掉本任务名下的所有块 */
            task_id_table[task_id - 3] = 0;         /* 归还 task_id，否则永不回收 */
            pal_printf("编译失败，错误行数:%d，错误信息:%s\n", build_return.err_line, build_return.err_msg);
            return JOB_ERROR_COMPILE_FAIL;
        }
    }
    //编译完成路径
    //首先经过加载器加载
    uint32_t vm_off = mm_alloc(task_id,sizeof(vm_t));
    if(vm_off == MM_FAIL){
        mm_reap(task_id);                           /* 统一清掉本任务名下的所有块 */
        task_id_table[task_id - 3] = 0;             /* 归还 task_id，否则永不回收 */
        pal_printf("内存分配失败\n");
        return JOB_ERROR_MM_ALLOC_FAIL;
    }
    vm_t* vm = (vm_t*)(mm_ram() + vm_off);
    if(loader_load_mem(vm,task_id,gc_enable,pimg,required+10,recursion_depth)!= 0){
        mm_reap(task_id);                           /* 连 loader 已申请的 region 一并回滚 */
        task_id_table[task_id - 3] = 0;             /* 归还 task_id，否则永不回收 */
        pal_printf("加载失败\n");
        return JOB_ERROR_LOAD_FAIL;
    }
    /* 镜像内容已全部拷进 region，这块缓冲该还给 MM：
     * 否则每个任务白占 10 KB，多任务时会把 100 KB 堆挤爆 */
    mm_free(mm_alloc_off);

    //加载完成，创建进程
    if(PS_Creat_Process_Ass(vm, "task", PS_PRIORITY_DEFAULT, task_id)!=0){
        pal_printf("创建进程失败\n");
        mm_reap(task_id);                           /* 连 loader 已申请的 region 一并回滚 */
        task_id_table[task_id - 3] = 0;             /* 归还 task_id，否则永不回收 */
        return JOB_ERROR_CREATE_PROCESS_FAIL;
    }
    return 0;
}

/* 归还 task_id 槽位。调度器在任务回收后通过回调调用它，
 * delete_Task 内部也复用它。 */
void job_release_id(uint32_t task_id){
    if (task_id >= JOB_TASK_ID_BASE && task_id < MAX_TASK_NUM) {
        task_id_table[task_id - JOB_TASK_ID_BASE] = 0;
    }
}

/* 主动销毁任务：回收内存 + 归还 task_id。
 * 注意：若该任务还在调度器链表里，这里会让链表留下悬空节点。
 * 正常流程下任务 HALT 后由调度器自动回收，CLI 不需要调用它。 */
void delete_Task(uint32_t task_id){
    mm_reap(task_id);
    job_release_id(task_id);
}

