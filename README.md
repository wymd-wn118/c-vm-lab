# c-vm-lab

一次实现练习：把「源码 → 字节码镜像 → 装载 → 多任务调度 → 回收」这条链路从头写了一遍。

**不是操作系统**，也不打算替代任何现有项目；只用于梳理各层如何协作、以及固定内存预算下资源怎么分配。全部代码运行在宿主进程里。

> 一次性练习，**不计划长期维护**。

**为什么这样设计**、以及几个"编译通过但跑起来才炸"的深层缺陷，记录在 [DESIGN.md](DESIGN.md)。

---

## 一、包含 / 不包含

**包含**


| 模块        | 内容                                                                  |
| ------------- | ----------------------------------------------------------------------- |
| 编译器      | C 子集 → `.osimg` 字节码镜像（词法 / 语法 / 代码生成 / 汇编 / 发射） |
| VM          | 32 位字节码解释器，寄存器、栈、堆（位图 + 定长池 + 句柄表）           |
| MM          | 100 KB 模拟物理内存，返回**偏移**、按 `task_id` 归属与回收            |
| Loader      | 解析 48 B 镜像头，按实际大小申请 prog / data / stack 三个窗口         |
| 调度器      | 10 级优先级链表、`PS_STEP` 步时间片、任务结束回收 + 回调              |
| CLI         | REPL、命令分发、任务创建编排（`job.c`）、驱动循环                     |
| PAL / DEBUG | 平台 I/O 抽象、分级日志（编译期裁剪）                                 |

**不包含**：进程与地址空间、虚拟内存与页表、中断与异常、特权级、文件系统、驱动模型、bootloader。

---

## 二、快速开始

当前验证环境为 Windows，使用 GCC（MinGW/MSYS2）或 MSVC。其他平台的 PAL 后端尚未提供完整 I/O。

使用 GCC 构建需要 `gcc` 和 `make` 在 PATH 中；Git Bash 本身不包含完整工具链，可使用 MSYS2 安装的工具：

```bash
make            # 生成 compiler_test.exe + cli.exe
./cli.exe
```

需要删除构建产物时再执行 `make clean`，不要在启动程序前清理。

MSVC 用 VS Code 任务 `build cli (cl)`（`/utf-8`），输出 `cli_msvc.exe`。需从已加载 MSVC 开发环境的终端启动 VS Code，让 `cl.exe` 可用。完整编译命令见 `makefile` 与 `.vscode/tasks.json`。

进去后跑一遍综合用例：

```
> run examples/complex.lang
```

它会输出 12 个数字（期望值与含义写在该文件头部注释里），覆盖数组、三种循环、双递归、多函数与嵌套调用。

---

## 三、架构与数据流

### 依赖方向（严格单向）

```
cli_win/cli.c        REPL / 命令分发 / 驱动主循环
      │
      └── cli_win/job.c   编排：分配 task_id → 编译 → 装载 → 注册
                 │
                 ├── compiler/                源码 → .osimg
                 ├── os/loader/               镜像 → VM 三个窗口
                 └── os/Process_Scheduler/    注册 + 调度 + 回收
                            │
                            ├── os/VM/       vm_step / vm_run / vm_reset
                            └── os/Memory_Manager/   100 KB 物理内存
                                       │
                                       └── PAL/    平台 I/O、编码、输入
```

**没有任何下层模块反向依赖上层。** 跨层访问只有一处：编排层 `job.c` 横向调用 compiler / loader / scheduler；回收则通过回调 `PS_SetReapCallback()` 把事件通知回编排层，而不是让调度器反向依赖它。

### 一次 `run` 的完整链路

```
源码文本
  → compiler_build()         编译，产出 .osimg（48B 头 + 代码段 + 数据段）
  → loader_load_mem()        解析头，mm_alloc 三个窗口，写入 vm_t
  → PS_Creat_Process_Ass()   挂进优先级链表，state = READY
  → PS_Step() 循环           每个任务每轮至多 PS_STEP 条指令
  → vm_step() 返回非 0      置 DEAD
  → PS_Delete_Process()      摘链 → mm_reap(task_id) → 释放节点 → 回调归还槽位
```

### 内存分层

```
┌─ 编译器堆 ─────────────────┐  task_id = 2，编译完 comp_reap_all() 清空
├─ 镜像 / 源码缓冲 ──────────┤  装载完立即释放
├─ vm_t + 三窗口 ────────────┤  每任务一个 task_id，结束时 mm_reap
└─ VM 内部堆 ────────────────┘  data 窗口内，位图/池/句柄表
        100 KB 模拟物理内存（MM）
```

---

## 四、目录结构

```
c-vm-lab/
├── code/
│   ├── compiler/                 编译器（compiler_main.c 是 compiler_test.exe 的 main）
│   ├── cli_win/                  cli.c / cli.h / job.c / job.h / test_multi_task.c
│   ├── os/
│   │   ├── VM/                   VM.c / VM_MM.c / VM.h
│   │   ├── Memory_Manager/       MM.c / MM.h
│   │   ├── loader/               Loader.c / Loader.h
│   │   └── Process_Scheduler/    Process_Scheduler.c / .h
│   ├── PAL/
│   │   ├── PAL.c                按 PLATFORM 宏 #include 对应平台实现
│   │   ├── windows/             WIN_std.c / WIN_syscall.c / WIN_ctx.c
│   │   └── generic/             PAL_generic.c（纯 C99 退化实现）
│   ├── DEBUG/                   分级日志
│   ├── config.h                 内存上限、日志阈值
│   ├── vm_isa.h                 指令集与 syscall 编号
│   └── vm_image.h               镜像格式（魔数 'OSV1'）
├── examples/complex.lang         综合用例
├── .vscode/                     构建任务（gcc / cl，均带 /utf-8 或 -finput-charset）
├── .gitignore
├── makefile
└── README.md
```

公开快照不包含 `bench/`、两处 `txt/` 或本地助手资料，也不携带开发仓库的旧提交历史。

### 接口在哪

头文件即接口定义，不在此重复声明：


|                                    |                                                 |
| ------------------------------------ | ------------------------------------------------- |
| 指令集与 syscall 编号              | `code/vm_isa.h`                                 |
| 镜像格式（48 B 头）                | `code/vm_image.h`                               |
| VM 对外接口                        | `code/os/VM/VM.h`                               |
| 内存管理接口（偏移 / 归属 / 回收） | `code/os/Memory_Manager/MM.h`                   |
| 装载器接口                         | `code/os/loader/Loader.h`                       |
| 调度器接口（含回收回调）           | `code/os/Process_Scheduler/Process_Scheduler.h` |
| 任务创建编排                       | `code/cli_win/job.h`                            |
| 平台抽象（全部 `pal_*`）           | `code/PAL/PAL.h`                                |

---

## 五、CLI 命令


| 命令                     | 作用                                                         |
| -------------------------- | -------------------------------------------------------------- |
| `run <file>`             | 读源码 → 编译 → 装载 → **一直跑到任务结束**（无轮数上限） |
| `spawn <file>`           | 同上，但只创建不运行，留待 `step` 推进                       |
| `runtext`                | 逐行输入源码，单独一行 `.` 结束，然后运行                    |
| `spawntext`              | 逐行输入源码，只创建不运行                                   |
| `runimg <file>`          | 直接装载 `.osimg`（跳过编译）                                |
| `list` / `ls`            | 列出 tid / 优先级 / 状态 / 名称                              |
| `step [n]`               | 手动推进 n 轮调度（默认 1），无上限                          |
| `kill <tid>`             | 标记 DEAD，走统一回收路径                                    |
| `mem`                    | 各任务 prog / data / stack 占用                              |
| `help` / `quit` / `exit` | 帮助 / 退出                                                  |

**长任务打断**：`run` 系列每轮用 `pal_input_ready()` 非阻塞检查一次，用户按任意键即可中断并回到提示符，任务仍留在调度器里可继续 `list` / `step` / `kill`。因此死循环程序不需要 Ctrl+C 杀掉整个进程。

### 多任务示例

```
> spawn examples/complex.lang
OK: 任务已创建但未运行（用 list 查看，step 推进）
> spawn examples/complex.lang
OK: 任务已创建但未运行（用 list 查看，step 推进）
> list
tid   prio  state    name
3     0     READY    task
4     0     READY    task
共 2 个任务
> step 1
已推进 1 轮，剩余任务 2
```

`step 1` 只推进 10 条指令，不够 `sum_all()` 跑完一轮循环，所以暂无数字输出、任务仍是 READY —— 这就是 `spawn` + `step` 逐步观察调度的用法；要看结果直接 `run`。

---

## 六、VM 语言参考

**支持**：局部/全局一维数组、`for` / `while` / `do-while`、`if`、递归、多参数函数、复合赋值、取模、自增、`print(int/float/char)`、结构体字段访问。

**不支持**：


| 类别   | 具体                                           |
| -------- | ------------------------------------------------ |
| 字面量 | 字符串字面量（`print("hi")` 报"未声明的变量"） |
| 运算符 | `&&` `||` `!`、位运算 `&` `|`                  |
| 控制流 | `break` / `continue`                           |
| 数组   | 作函数参数、初始化列表 `{}`、二维数组          |
| 表达式 | 连续二元运算 `a + f(7) + c` 报 `expected ';'`  |

只有 `print`，没有 `printf`。

---

## 七、资源模型


| 项               | 值                                                                         |
| ------------------ | ---------------------------------------------------------------------------- |
| 物理内存         | **100 KB**（`MM_RAM_BYTES`）                                               |
| 任务槽位         | **13 个**（`JOB_TASK_SLOTS`，`task_id` 3..15）                             |
| `task_id` 保留号 | 0 = FREE，1 = SYSTEM，2 = COMPILER                                         |
| 单任务窗口       | 约 **7.4 KB**（实测 prog 64 + data 3368 + stack 3983 B，按镜像头按需分配） |
| 优先级 / 时间片  | 10 级 / `PS_STEP = 10` 条指令每轮                                          |
| 镜像魔数         | `'OSV1'`（`VM_IMG_MAGIC`），**不要改**，改了旧 `.osimg` 全部失效           |

100 KB / 7.4 KB ≈ 13，与槽位数自洽。

`mm_alloc` 返回的是**偏移**而非指针，需 `mm_ram() + off` 才能用；失败返回 `MM_FAIL = 0xFFFFFFFF`，必须检查。

---

## 八、已知限制


| 项                         | 说明                                                                                                                                     |
| ---------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| **严格优先级抢占**         | 高优先级只要有 READY 任务，低优先级就不执行；同级才轮转。这是正常的调度策略，不是缺陷                                                    |
| **多任务输出交错**         | `print(x)` 编译成两条 syscall（数字 + 换行），时间片可在中间切断，出现 `15` 粘行或多余空行。无锁共享 stdout 的正常并发现象，不是内存错误 |
| **连续二元运算**           | `a + f(7) + c` 解析失败（见第六节）                                                                                                      |
| **非阻塞输入范围**         | 只用于「打断长任务」；命令行仍由 `pal_gets` 阻塞读取。Ctrl+C 依然会终止进程                                                              |
| **`PS_Main()` 无退出**     | 死循环；CLI 不用它                                                                                                                       |
| **严格优先级有无用武之地** | 缺少等待 / 让出指令，任务不会主动阻塞，所以目前所有任务都用默认优先级 0                                                                  |

---

## 九、测试

```bash
./cli.exe
> run examples/complex.lang      # 12 项断言，见第二节输出
```

`code/cli_win/test_multi_task.c` 是唯一固化的冒烟测试，编译命令见该文件头部注释。覆盖三件事：

1. 连续创建 3 个任务（各自独立编译、独立内存窗口）
2. 多个 VM 是否交替执行（时间片轮转，而非一个跑完再跑下一个）
3. 任务 HALT 后能否被 `PS_Delete_Process` 回收干净（剩余任务数归零）

**以下项目是手工验证过的，但没有固化成脚本**：`PS_Init` 幂等、非法 `task_id` / `priority` 拒绝、`vm_reset` 复用、13 个槽位循环复用 20 轮、`examples/complex.lang` 的 12 项断言（期望值见该文件头部注释）。

---

## 十、开发约定

- **源码统一 UTF-8 + BOM**。MSVC 凭 BOM 识别 UTF-8；无 BOM 时会按 GBK(936) 读，中文注释字节错位会把注释提前截断，引发成片的"未声明标识符"错误。
- **平台相关代码只在 `code/PAL/<platform>/`**。其余模块一律走 `pal_*` 接口；新增平台只需在 `PAL.c` 加一个 `#elif` 分支。
- `PAL.c` 用 `#include` 包含平台实现文件，`makefile` 里已为 `build/PAL.o` 显式声明这些依赖 —— 否则改了 `WIN_std.c` 不会重建 `PAL.o`，会链接到过期符号。
- 错误判定一律靠返回值 / 错误码，**不依赖 `DBG` 输出**（日志可在编译期整体关闭）。

---

## 十一、许可

[MIT](LICENSE)。学习、参考、修改随意，不提供任何担保。
