/******************************************************************************
 * 文件名:   Loader.c
 * 作者:     WoYongMengDie
 * 创建时间: 2026-7-24
 * Desc:    Loader 实现——任务创建者（task creator）。
 *          职责：把 .osimg 变成可运行的 vm_t 执行体，负责装载、执行指令、
 *          处理运行期内存细节（按约定协调 MM 要窗口 / VM 初始化与执行）。
 *          分工：MM 只管“谁占了哪片 RAM 的段”，VM 只管“取指执行”，
 *          Loader 负责“读镜像 → 拆头 → 向 MM 要窗口 → 初始化 VM/栈 → 跑起来”。
 * 版本:     0.0.2
 ******************************************************************************/
#include "Loader.h"
#include "PAL.h"        /* loader 所需：文件 IO / 堆分配 / 字节拷贝全部走 pal_* */
#include "DEBUG.h"      /* 分级日志：DBG(DEBUG_COMP_LOADER, ...) */

/* ===================== 可选 profiling 开关 =====================
 * 让上层在加载前调 loader_enable_profiling() 打开，loader 会把它透传给 vm，
 * 使用方据此做一次采样，记录栈/堆峰值与步数，便于评估 F407 100KB 内是否够用。
 * budget=0 表示不限步数、跑到自然停止（受 VM_RUN_MAX_STEPS 兜底），
 * 非 0 为最大预算（到点即停，返回 VM_PROFILE_DONE）。 */
static uint8_t  g_profiling = 0;
static uint32_t g_profile_budget = 0;
void loader_enable_profiling(uint32_t budget) { g_profiling = 1; g_profile_budget = budget; }

/*
 * @brief  创建任务：向 MM 申请三块窗口并初始化 CPU/栈，但不装载代码/数据。
 * @param  vm_t* vm        当前虚拟机实例
 * @param  uint32_t task_id 任务归属者标识
 * @param  uint8_t gc_enable 1=开启 GC，0=关闭 GC
 * @return uint8_t 0=VM_OK 成功；VM_LOAD_ERR_NOMEM=窗口申请失败
 * @note   三块窗口大小写死为 config 值（VM_PC_SIZE/VM_DATA_SIZE/VM_STACK_SIZE），
 *         与 loader_spawn 的行为保持一致（spawn 的栈可按 recursion_depth 动态算）。
 *         此处固定为 VM_STACK_SIZE，不做 mm_alloc 失败后的降级检查（见 next_steps.md 条目 C）。 */
uint8_t loader_create_task(vm_t* vm, uint32_t task_id, uint8_t gc_enable)
{
    mm_init();                                   /* 确保 MM 已就绪（物理 RAM 清零、头节点建立） */

    /* 1) 向 MM 申请三块窗口，task_id 只用于归属记账（MM 不解释语义） */
    uint32_t c = mm_alloc(task_id, VM_PC_SIZE);
    uint32_t d = mm_alloc(task_id, VM_DATA_SIZE);
    uint32_t s = mm_alloc(task_id, VM_STACK_SIZE);
    if (c == MM_FAIL || d == MM_FAIL || s == MM_FAIL) {
        /* MM_FAIL 是 0xFFFFFFFF，直接做 mm_ram() + MM_FAIL 会得到 ram-1，
         * VM 就在错误基址上跑。故把已经申请成功的窗口释放掉，避免重试时
         * 永久失血，然后再报失败。 */
        if (c != MM_FAIL) mm_free(c);
        if (d != MM_FAIL) mm_free(d);
        if (s != MM_FAIL) mm_free(s);
        DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR,
            "create_task FAILED: task=%u windows exhausted (code=%s data=%s stack=%s)\n",
            task_id,
            c == MM_FAIL ? "FAIL" : "ok",
            d == MM_FAIL ? "FAIL" : "ok",
            s == MM_FAIL ? "FAIL" : "ok");
        return VM_LOAD_ERR_NOMEM;
    }

    /* 2) 把窗口偏移换算成真实指针：vm 里的“内存地图”base 指向 MM 的物理 RAM */
    vm->prog.base  = mm_ram() + c;  vm->prog.size = VM_PC_SIZE;  vm->prog.used = 0;

    vm->data.base  = mm_ram() + d;  vm->data.size = VM_DATA_SIZE; vm->data.used = 0;

    vm->stack.base = mm_ram() + s;  vm->stack.size = VM_STACK_SIZE; vm->stack.used = 0;

    /* 3) 窗口就绪后初始化 CPU/栈；vm_init 不碰 MM，堆由 vm_mm_init 单独建 */
    vm_init(vm);

    vm_mm_init(vm);                             /* 无镜像头：按默认池大小建堆（测试桩/直接调用场景） */

    /* 4) 按任务配置 GC：gc_enable=1 稳定性优先（开 GC），0 实时性优先（关 GC） */
    vm_mm_set_gc(vm, gc_enable);
    return VM_OK;
}

/* ===================== 执行循环 + OOM 重试 =====================
 * process_run 是 Loader 内的“运行到底”循环：一直跑到停机/出错为止，
 * 一旦撞上 VM_OOM_ERROR（变长区不够），就向 MM 申请扩大 data 窗口再试。 */
#define OS_DATA_GROW_STEP  4000u     /* 每次向上追加的 data 区字节数 */
#define OS_OOM_MAX_RETRY   16u       /* 防止无限重试的安全上限 */

uint8_t process_run(vm_t* vm, uint32_t task_id)
{
    /* 最多重试 OS_OOM_MAX_RETRY 次，超出即认定失败，避免死循环 */
    for (uint32_t attempt = 0; attempt < OS_OOM_MAX_RETRY; attempt++) {
        uint8_t err = vm_run(vm);
        if (err != VM_OOM_ERROR) return err;   /* 正常停机 / 其它错误直接返回调用方 */

        /* VM 报变长区不足：按当前窗口位置和大小向 MM 申请扩容 */
        uint32_t old_start = (uint32_t)(vm->data.base - mm_ram());
        uint32_t old_size  = vm->data.size;
        uint32_t new_start = mm_grow_region(task_id, old_start, old_size, OS_DATA_GROW_STEP);
        if (new_start == MM_FAIL) {                       /* 整片 RAM 再挤不出空间：停机 */
            DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR,
                "OOM grow FAILED: task=%u old=%u B need +%u B (no free block anywhere)\n",
                task_id, old_size, (unsigned)OS_DATA_GROW_STEP);
            return VM_FATAL_ERROR;
        }

        if (new_start == old_start) {
            /* 路线1 原地加长：base 不变，只需 VM_MM 重建内部结构 */
            vm_mm_grow_inplace(vm, old_size + OS_DATA_GROW_STEP);
        } else {
            /* 路线2 重定位：MM 已把数据拷到新段，VM_MM 按新地址重建结构与位图 */
            vm_mm_relocate(vm, mm_ram() + new_start, old_size + OS_DATA_GROW_STEP);
        }
        DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_WARN,
            "OOM retry %u/%u: data %u B -> +%u B (task=%u)\n",
            attempt + 1, (unsigned)OS_OOM_MAX_RETRY, old_size,
            (unsigned)OS_DATA_GROW_STEP, task_id);
        /* vm_run 遇 OOM 时已把 PC 退回到当前指令，循环重试即可重跑该 ALLOC 指令 */
    }
    DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR,
        "OOM retry exhausted after %u attempts (task=%u)\n",
        (unsigned)OS_OOM_MAX_RETRY, task_id);
    return VM_OOM_ERROR;   /* 重试次数用尽仍失败：报错退出 */
}

/* ===================== .loading 清单解析（自带小工具） =====================
 * 不引入 <string.h>：PAL 未提供 strchr/strlen 等等价物时，Loader 自带几个
 * 字符串/路径工具，统一用 ld_ 前缀，避免与 VM/MM 的符号冲突。 */
#define LD_MAX_PATH 512u

/* ld_strchr：与标准 strchr 等价——返回 s 中首次出现字符 ch 的指针，找不到返回 NULL */
static char* ld_strchr(char* s, int ch) {
    char* p = s;
    while (*p) { if (*p == (char)ch) return p; p++; }
    return NULL;
}

/* ld_trim：原地去掉首尾空白（空格 / 制表符 / 回车），返回去空白后的起点 */
static char* ld_trim(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;   /* 跳过开头空白 */
    if (!*s) return s;                                    /* 空串直接返回 */
    char* e = s + pal_strlen(s) - 1;                      /* 定位末字符 */
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\r')) *e-- = 0;  /* 截断尾部空白 */
    return s;
}

/* ld_dir：取 .loading 路径的目录部分（保留结尾 '/'），写入 dir；供拼接 image 相对路径 */
static void ld_dir(const char* path, char* dir) {
    /* NULL = 还没遇到分隔符。旧实现从 path 起步，导致纯文件名（无目录）时
     * n = 1、dir 变成文件名的第一个字符："s1_a.osimg" → dir "s" → "s/s1_a.osimg"。 */
    const char* sl = NULL;
    for (const char* p = path; *p; p++) if (*p == '/' || *p == '\\') sl = p;  /* 记录最后一个路径分隔符 */
    if (!sl) { dir[0] = 0; return; }             /* 纯文件名：没有目录部分 */
    uint32_t n = (uint32_t)(sl - path) + 1;
    if (n > LD_MAX_PATH - 1) n = LD_MAX_PATH - 1;
    pal_memcpy(dir, path, n);
    dir[n] = 0;
}

/* ld_join：拼接 dir+name；name 为绝对路径时直接复制。返回新串，由调用方 pal_free */
static char* ld_join(const char* dir, const char* name) {
    if (name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':'))   /* 绝对路径：直接复制 */
        return pal_strdup(name);
    uint32_t dn = (uint32_t)pal_strlen(dir);
    uint32_t nn = (uint32_t)pal_strlen(name);
    if (dn == 0) return pal_strdup(name);        /* 无目录：不要在前面补 '/' */
    char* out = (char*)pal_malloc(dn + nn + 2);
    if (!out) return NULL;
    pal_memcpy(out, dir, dn);
    out[dn] = '/';
    pal_memcpy(out + dn + 1, name, nn);
    out[dn + 1 + nn] = 0;
    return out;
}

/* ===================== .loading 解析：key=value → vm_task_cfg_t =====================
 * 清单格式：每行 “key=value”，# 开头或 # 之后为注释，空行 / 无 = 的行跳过。
 * image 的相对路径按 .loading 所在目录解析。 */
/*
 * @brief  读取 .loading 清单，逐行解析 key=value 填入 vm_task_cfg_t。
 * @param  const char* loading_path  .loading 清单文件路径
 * @param  vm_task_cfg_t* cfg        输出参数：解析结果（调用方负责回收 image）
 * @return uint8_t 0 表示解析成功，非 0 表示失败（缺 image / 文件不可读等）
 * @note   支持的键：image/gc_enable/recursion_depth/profiling/profile_steps/stack_override/ram_size。 */
static uint8_t parse_loading(const char* loading_path, vm_task_cfg_t* cfg)
{
    uint32_t sz;
    char* raw = (char*)pal_read_file(loading_path, &sz);   /* 读入整个清单文件 */
    if (!raw) {
        DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR, "loading unreadable: %s\n", loading_path);
        return VM_LOAD_ERR_OPEN;
    }
    pal_memset(cfg, 0, sizeof(*cfg));                       /* 清零：未写字段 = 0 / 默认值 */
    cfg->task_id   = MM_TASK_SYSTEM;                        /* 默认归系统任务 */
    cfg->gc_enable = 1;                                     /* 默认开 GC（稳定性优先） */
    cfg->ram_size  = (uint32_t)CONFIG_RAM_SIZE * 8u;        /* 默认 = 项目总内存 */
    char dir[LD_MAX_PATH];
    ld_dir(loading_path, dir);                              /* 取清单所在目录，用于拼 image 路径 */

    char* p = raw;
    while (*p) {                                            /* 逐行解析 */
        char* nl = ld_strchr(p, '\n');
        char* line;
        if (nl) { *nl = 0; line = p; p = nl + 1; }          /* 切行：截断成一行，p 指向下一行 */
        else    { line = p; p += pal_strlen(p); }           /* 最后一行无换行，直接走到串尾 */

        char* hash = ld_strchr(line, '#');                  /* 去注释：把 # 处截断 */
        if (hash) *hash = 0;

        char* eq = ld_strchr(line, '=');                    /* 切出 key/value 分界 */
        if (!eq) continue;                                  /* 无 = 视为注释/空行，跳过 */
        *eq = 0;                                            /* 把 = 换成字符串结束，line 即为 key */
        char* key = ld_trim(line);                          /* key = = 之前部分，去空白 */
        char* val = ld_trim(eq + 1);                        /* val = = 之后部分，去空白 */

        /* 逐字段匹配 key，把 val 转成对应类型写入 cfg */
        if      (pal_strcmp(key, "image") == 0) {
            char* joined = ld_join(dir, val);               /* 按目录拼出完整路径 */
            if (joined) { cfg->image = joined; cfg->image_owned = 1; }  /* owned=1：由本层负责 pal_free */
        }
        else if (pal_strcmp(key, "task_id") == 0)         cfg->task_id = pal_strtoul(val, NULL, 10);
        else if (pal_strcmp(key, "gc_enable") == 0)       cfg->gc_enable = (uint8_t)pal_strtoul(val, NULL, 10);
        else if (pal_strcmp(key, "recursion_depth") == 0) cfg->recursion_depth = pal_strtoul(val, NULL, 10);
        else if (pal_strcmp(key, "profiling") == 0)       cfg->profiling = (uint8_t)pal_strtoul(val, NULL, 10);
        else if (pal_strcmp(key, "profile_steps") == 0)   cfg->profile_steps = pal_strtoul(val, NULL, 10);
        else if (pal_strcmp(key, "stack_override") == 0)  cfg->stack_override = pal_strtoul(val, NULL, 10);
        else if (pal_strcmp(key, "ram_size") == 0)        cfg->ram_size = pal_strtoul(val, NULL, 10);
    }
    pal_free(raw);                                          /* 清单内容用完即释放 */
    if (!cfg->image) {                                      /* 必须有 image，否则失败 */
        DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR, "loading has no image= key: %s\n", loading_path);
        return VM_LOAD_ERR_NOIMAGE;
    }
    DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_DEBUG,
        "cfg: image=%s task=%u gc=%u depth=%u stack_override=%u profiling=%u steps=%u\n",
        cfg->image, cfg->task_id, (unsigned)cfg->gc_enable, cfg->recursion_depth,
        cfg->stack_override, (unsigned)cfg->profiling, cfg->profile_steps);
    return 0;
}

/* ===================== data 窗口最小尺寸估算 =====================
 * VM_MM 的布局约束（详见 vm_mm_init_layout）：
 *   bitmap_off = total - ceil(total/8)      位图贴在 total 末尾
 *   var_break  = hsz + 静态池 + ht_cap*8    header + 池 + 句柄表
 * 要使 bitmap_off >= var_break，需 total >= ceil((hsz + 静态池 + ht_cap*8) * 8 / 7)；
 * 再留 25% 余量（句柄表从 256 涨到 512 时变长区也会被挤占）。
 * 此前 data 窗口写死 VM_DATA_SIZE(20 KB)，简单程序只用几十字节也占满 20 KB；
 * 有了这个估算，小程序的 data 窗口可降到 3 KB 左右，同样 100 KB 总内存能放更多任务。 */
/*
 * @brief  计算 data 窗口的最小可用尺寸（静态数据 + VM_MM 自身开销）。
 * @param  static_bytes 镜像头里数据段的字节数（静态全局 + 静态变长区）
 * @return data 窗口应申请的字节数
 * @note   vm_init() 已不再按默认池布置（见 VM.c 说明），此处只按镜像头
 *         的真实静态段 + 位图开销估算；ht_cap 取 MM_HT_INIT_CAP（VM_MM.c 私有宏 256），
 *         其上限 VM_MM_MAX_HT_CAP(512) 由 +25% 余量覆盖。 */
static uint32_t loader_calc_data_min(uint32_t static_bytes)
{
    uint32_t hsz  = (uint32_t)sizeof(vm_mm_hdr_t);
    uint32_t ht   = 256u * 8u;                    /* 句柄表初始容量 256 槽 × 8 B */
    uint32_t x    = hsz + static_bytes + ht;
    uint32_t need = (x * 8u + 6u) / 7u;           /* 向上取整 8x/7 */
    need += need / 4u;                             /* +25% 余量 */
    need = (need + 3u) & ~3u;
    if (need < static_bytes) need = static_bytes;  /* 兜底：至少装得下静态段 */
    return need;
}
/*
 *@brief  解析镜像头，获取镜像文件的代码段和数据段的长度
 *@param  raw 镜像文件的指针
 *@param  sz  镜像文件的长度
 *@param  cfg 输出参数：解析结果（调用方负责回收 image）
 *@param  vm  虚拟机对象
 *@return 0 表示解析成功，非 0 表示失败（缺 image / 文件不可读等
 *@node 使用者需要释放raw
 */
static uint8_t loader_prepare(const uint8_t* raw,uint32_t sz,const vm_task_cfg_t* cfg,vm_t* vm){
    if (sz < 48) {                                          /* 连头部都不足 48B：直接失败 */
        DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR,
            "image too small: %s is %u B (< 48 B header)\n", cfg->image, sz);
        return VM_LOAD_ERR_HEADER;
    }
    /* --- 2) 解析头：magic(4) + layout(36) + code_len(4) + data_len(4) = 48B --- */
    uint32_t magic;
    vm_static_layout_t l;
    uint32_t code_len, data_len;
    pal_memcpy(&magic,    raw,        4);   /* 魔数 OSV1：识别文件类型 */
    pal_memcpy(&l,        raw + 4,    36);  /* 9 个 u32 布局字段（含 max_frame_size） */
    pal_memcpy(&code_len, raw + 40,   4);   /* 代码段字节数 */
    pal_memcpy(&data_len, raw + 44,   4);   /* 数据段字节数 */
    /* 头部校验：魔数必须是 OSV1；且 48+code+data 不超过文件实际大小（防截断） */
    if (magic != VM_IMG_MAGIC ||
        (uint32_t)(48 + (uint32_t)code_len + (uint32_t)data_len) > sz) {
        DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR,
            "bad image: %s magic=0x%08X (want 0x%08X), need %u B but file has %u B\n",
            cfg->image, magic, (unsigned)VM_IMG_MAGIC,
            (unsigned)(48 + (uint32_t)code_len + (uint32_t)data_len), sz);

        return VM_LOAD_ERR_HEADER;   /* 魔数或长度不对 */
    }

    /* --- 3) 申请代码/数据缓冲，从 raw 拷出后立即释放原文件（省 RAM） --- */
    //uint8_t* code = (uint8_t*)pal_malloc(code_len ? code_len : 1);  /* 防 0 长度申请 */
    //uint8_t* data = (uint8_t*)pal_malloc(data_len ? data_len : 1);
    //如果超过最长代码段限制，直接报错
    if (code_len > (uint32_t)VM_PC_SIZE || data_len > (uint32_t)VM_DATA_SIZE) {
        DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR,
            "bad image: %s code_len=%u (max %u) data_len=%u (max %u)\n",
            cfg->image, code_len, (unsigned)VM_PC_SIZE,
            data_len, (unsigned)VM_DATA_SIZE);

        return VM_LOAD_ERR_HEADER;
    }

    //计算需要的固定长度字节池尺寸，如果太长则直接报错
    {
        uint64_t pool_bytes = (uint64_t)l.pool1_cnt
                            + (uint64_t)l.pool2_cnt * 2u
                            + (uint64_t)l.pool4_cnt * 4u;
        if (pool_bytes > (uint64_t)VM_DATA_SIZE) {
            DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR,
                "bad image: %s pool bytes %u exceed data window %u "
                "(cnt: 1B=%u 2B=%u 4B=%u)\n",
                cfg->image, (unsigned)pool_bytes, (unsigned)VM_DATA_SIZE,
                l.pool1_cnt, l.pool2_cnt, l.pool4_cnt);
            return VM_LOAD_ERR_HEADER;
        }
    }
    //pal_memcpy(code, raw + 48,                code_len);     /* 代码段从偏移 48 起 */
    //pal_memcpy(data, raw + 48 + code_len,     data_len);     /* 数据段紧随代码段之后 */

    /* --- 4) 算栈窗口：override > depth×max_frame+余量 > 默认 VM_STACK_SIZE --- */
    uint32_t stack_bytes;
    /* 栈窗口大小优先级：override > 按递归深度估算 > 默认 VM_STACK_SIZE */
    if (cfg->stack_override) {
        stack_bytes = cfg->stack_override;            /* 清单强制指定：按值给 */
    } else if (cfg->recursion_depth) {
        /* 按帧大小估算：frame*depth 覆盖所有递归栈帧，再加上被调方栈帧的
         * 余量（按帧大小的 1/4），最后加一点对齐/保存寄存器的余量防越界。 */
        uint32_t frame = l.max_frame_size ? l.max_frame_size : 256u;
        stack_bytes = frame * cfg->recursion_depth
                    + frame / 4u + 128u;
        /* 上界钳制：算得过大时退到默认上限，别把 RAM 吃光 */
        if (stack_bytes > VM_STACK_SIZE) stack_bytes = VM_STACK_SIZE;
    } else {
        stack_bytes = VM_STACK_SIZE;                  /* 既无 override 也无深度：用默认 */
    }
    DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_DEBUG,
        "stack: override=%u depth=%u frame=%u -> %u B\n",
        cfg->stack_override, cfg->recursion_depth, l.max_frame_size, stack_bytes);

    /* --- 5) 向 MM 申请三个窗口（prog/data 按实际需要算，栈按上面算出的值） --- */
    mm_init();                                   /* 确保 MM 已就绪（物理 RAM 清零、头节点建立） */

    /* prog 只需装下 code_len，data 需装下静态段 + VM_MM 的 header/池/句柄表/位图。
     * 若按 VM_PC_SIZE/VM_DATA_SIZE 写死，fib 这种 508 B 的小程序也要占 30 KB；
     * 按镜像头精确计算后，同样 100 KB 总内存能放更多任务。 */
    uint32_t code_bytes = ((uint32_t)code_len + 3u) & ~3u;
    if (code_bytes < 64u) code_bytes = 64u;                        /* 最小 64 B */
    if (code_bytes > (uint32_t)VM_PC_SIZE) code_bytes = (uint32_t)VM_PC_SIZE;

    uint32_t data_bytes = loader_calc_data_min((uint32_t)data_len);
    if (data_bytes > (uint32_t)VM_DATA_SIZE) data_bytes = (uint32_t)VM_DATA_SIZE;

    uint32_t c = mm_alloc(cfg->task_id, code_bytes);
    uint32_t d = mm_alloc(cfg->task_id, data_bytes);
    uint32_t s = mm_alloc(cfg->task_id, stack_bytes);
    //如果分配失败就释放已经分配成功的内容并且直接return 
    if (c == MM_FAIL || d == MM_FAIL || s == MM_FAIL) {
        if (c != MM_FAIL) mm_free(c);
        if (d != MM_FAIL) mm_free(d);
        if (s != MM_FAIL) mm_free(s);
        DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR,
            "window alloc FAILED: %s code=%s data=%s stack=%s (%u B)\n",
            cfg->image,
            c == MM_FAIL ? "FAIL" : "ok",
            d == MM_FAIL ? "FAIL" : "ok",
            s == MM_FAIL ? "FAIL" : "ok", stack_bytes);
        return VM_LOAD_ERR_NOMEM;
    }

    vm->prog.base  = mm_ram() + c;  vm->prog.size = code_bytes;  vm->prog.used = 0;
    vm->data.base  = mm_ram() + d;  vm->data.size = data_bytes;  vm->data.used = 0;
    vm->stack.base = mm_ram() + s;  vm->stack.size = stack_bytes; vm->stack.used = 0;
    DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_DEBUG,
        "windows: code=%u B (image %u) data=%u B (static %u) stack=%u B (task=%u)\n",
        code_bytes, (unsigned)code_len, data_bytes, (unsigned)data_len,
        stack_bytes, cfg->task_id);

    /* --- 6) VM 初始化 + 堆布置 --- */
    vm_init(vm);
    vm->max_frame_size = l.max_frame_size;   /* 镜像头里的最大栈帧（供栈溢出检测用） */
    if (cfg->profiling) { vm->profiling = 1; vm->profile_budget = cfg->profile_steps; }
    vm_mm_init_layout(vm, &l);               /* 按镜像头把 data 区布置成堆（池/句柄表/变长区/位图） */
    vm_mm_set_gc(vm, cfg->gc_enable);        /* 设置 GC 开关 */

    /* --- 7) 写入静态数据 + 装载代码 --- */
    if (data_len) pal_memcpy(vm->data.base + (uint32_t)vm->cpu.R[VM_POOL1], raw + 48 + code_len, data_len);  /* 数据从 1B 池基址开始铺 */
    vm_load(vm,raw + 48, code_len);             /* 代码拷进 prog 窗口，PC 归零 */

    /* --- 8) 释放临时缓冲并运行（步骤 B：本层注册到 Process 调度器后，这里再执行） --- */
    DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_INFO,
        "load ok: image=%s code=%u B data=%u B -> run (task=%u, gc=%u)\n",
        cfg->image, code_len, data_len, cfg->task_id, (unsigned)cfg->gc_enable);
    
    return VM_OK;
}

/* ===================== 内部 spawn：把 cfg 变成窗口并跑起来 =====================
 * 这里是“加载 → 运行”的唯一转换点：所有清单 / 直连入口最后都收敛到这里。
 * 步骤：读镜像 → 拆头 → 要 code/data 与 栈 → MM 申请窗口 →
 *       VM 初始化 + 堆布置 → 装载代码/数据 → process_run 执行。 */
/*
 * @brief  内部实现：根据 cfg 申请窗口、装载代码/数据并执行。
 * @param  vm_t* vm             当前虚拟机实例
 * @param  const vm_task_cfg_t* cfg  任务配置（image/task_id/gc_enable/栈大小等）
 * @return uint8_t 返回码（0 表示成功，非 0 为 VM_xxx_ERROR / VM_FATAL_ERROR）
 * @note   所有对外入口（loader_load_image / loader_load_loading_file）最终都收敛于此。 */
static uint8_t loader_spawn(vm_t* vm, const vm_task_cfg_t* cfg)
{
    /* --- 1) 读入 .osimg 文件 --- */
    uint32_t sz;
    uint8_t* raw = (uint8_t*)pal_read_file(cfg->image, &sz);
    if (!raw) {
        DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR, "image unreadable: %s\n", cfg->image);
        return VM_LOAD_ERR_OPEN;
    }
    
    
    uint8_t loader_prepare_returnnumber = loader_prepare(raw,sz,cfg,vm);
    if(loader_prepare_returnnumber != VM_OK){
        pal_free(raw);
        return loader_prepare_returnnumber;
    }
    pal_free(raw);
    uint8_t err = process_run(vm, cfg->task_id);
    if (err != VM_OK) {
        DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_ERROR,
            "task FAILED: image=%s err=%u (%s)\n", cfg->image, err, vm_err_name(err));
    } else {
        DBG(DEBUG_COMP_LOADER, DEBUG_LEVEL_INFO, "task done: image=%s ok\n", cfg->image);
    }

    return err;
}

/* ===================== 对外入口 ===================== */
/*
 * @brief  直接给定 .osimg 路径 + task_id + gc_enable 即可运行（内部构造最小 cfg 交给 loader_spawn）。
 * @param  vm_t* vm           当前虚拟机实例
 * @param  const char* path   .osimg 文件路径
 * @param  uint32_t task_id   任务归属者标识
 * @param  uint8_t gc_enable  1=开启 GC，0=关闭 GC
 * @return uint8_t 返回码（0 表示成功，非 0 为 VM_xxx_ERROR）
 * @note   该 cfg 只按默认 recursion_depth=64 估算栈（上限 VM_STACK_SIZE），不支持 stack_override。 */
uint8_t loader_load_image(vm_t* vm, const char* path, uint32_t task_id, uint8_t gc_enable)
{
    vm_task_cfg_t cfg;
    pal_memset(&cfg, 0, sizeof(cfg));
    cfg.task_id   = task_id;
    cfg.gc_enable = gc_enable;
    cfg.image     = path;
    cfg.ram_size  = (uint32_t)CONFIG_RAM_SIZE * 8u;
    /* 默认递归深度：栈按 max_frame_size * depth + frame/4 + 128 估算。
     * 取 64 层：一般场景足够（fib(20) 才 20 层），实际用量约 4.2 KB，写不满 16 KB 上限；
     * 更深的递归请在 .loading 清单里用 recursion_depth 显式指定 —— 栈不够时 VM 会
     * 报 VM_STACK_OVERFLOW_ERROR，明确报错总比静默溢出好。 */
    cfg.recursion_depth = 64u;
    if (g_profiling) { cfg.profiling = 1; cfg.profile_steps = g_profile_budget; }  /* 透传全局 profiling */
    return loader_spawn(vm, &cfg);
}
/*
 *@brief 只输入uint8_t*格式指令方式的输入指令
 *@param vm_t* vm 当前虚拟机实例
 *@param uint32_t task_id 任务归属者标识
 *@param uint8_t gc_enable 1=开启 GC，0=关闭 GC
 *@param uint8_t* raw 指令内容
 *@param uint32_t sz 指令长度
 *@param uint32_t recursion_depth 递归深度
 *@return uint8_t 返回码（0 表示成功，非 0 为 VM_xxx_ERROR）
 */
uint8_t loader_load_mem(vm_t* vm,uint32_t task_id,uint8_t  gc_enable,uint8_t* raw,uint32_t sz,uint32_t recursion_depth){
    vm_task_cfg_t cfg;
    pal_memset(&cfg, 0, sizeof(cfg));
    cfg.task_id   = task_id;
    cfg.gc_enable = gc_enable;
    /* mem load: no file path. Use a placeholder so that DBG's "%s"
     * never receives NULL (some CRT implementations crash on it). */
    cfg.image     = "<mem>";
    cfg.recursion_depth = recursion_depth;
    cfg.ram_size  = (uint32_t)CONFIG_RAM_SIZE * 8u;
    return loader_prepare(raw, sz, &cfg, vm);
}

/*
 * @brief  对外入口：读取 .loading 清单得到完整 cfg 后交给 loader_spawn 执行。
 * @param  vm_t* vm             当前虚拟机实例
 * @param  const char* loading_path  .loading 清单文件路径
 * @return uint8_t 返回码（0 表示成功，非 0 为 VM_xxx_ERROR）
 * @note   image 字符串可能是本层分配的（image_owned），执行完必须 pal_free 回收。 */
uint8_t loader_load_loading_file(vm_t* vm, const char* loading_path)
{
    vm_task_cfg_t cfg;
    uint8_t perr = parse_loading(loading_path, &cfg);
    if (perr != VM_OK) return perr;   /* 透传清单解析的错误码（不可读 / 缺 image 等） */
    uint8_t rc = loader_spawn(vm, &cfg);
    if (cfg.image_owned && cfg.image) pal_free((void*)cfg.image);
    return rc;
}
