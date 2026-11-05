# ---- 1. 工具和编译参数 ----
CC     = gcc
# 源码统一 UTF-8：显式钉死输入/执行字符集，不再随系统 locale 变化。
CFLAGS = -std=gnu99 -O2 -Wall \
         -finput-charset=UTF-8 -fexec-charset=UTF-8 \
         -I code -I code/os -I code/os/VM -I code/os/Memory_Manager \
         -I code/os/loader -I code/os/Process_Scheduler \
         -I code/cli_win -I code/DEBUG -I code/PAL -I code/compiler

# clean 用 rm（sh / Git Bash / MSYS2 环境）。cmd 下没有 rm，请改用 sh 运行 make。
RM    ?= rm -f
BUILD  = build

# ---- 2. 源文件 ----
# KERNEL：OS 内核 + 任务编排层（job.c）
KERNEL = $(wildcard code/os/VM/*.c) \
         $(wildcard code/os/loader/*.c) \
         $(wildcard code/os/Memory_Manager/*.c) \
         $(wildcard code/os/Process_Scheduler/*.c) \
         code/cli_win/job.c \
         code/PAL/PAL.c \
         code/DEBUG/DEBUG.c

COMPILER = $(wildcard code/compiler/*.c)

# ---- 3. 目标文件：全部收进 build/，只用文件名区分 ----
KERNEL_OBJ   = $(addprefix $(BUILD)/,$(notdir $(KERNEL:.c=.o)))
COMPILER_OBJ = $(addprefix $(BUILD)/,$(notdir $(COMPILER:.c=.o)))
# compiler_main.c 自带 main()，cli.c 也有 main()，两者不能同时链接
COMPILER_LIB_OBJ = $(filter-out $(BUILD)/compiler_main.o,$(COMPILER_OBJ))
CLI_OBJ      = $(BUILD)/cli.o

# VPATH：目标旁边找不到 .c 时，make 会去这些目录里找
VPATH = $(sort $(dir $(KERNEL) $(COMPILER) code/cli_win/cli.c))

# ---- 4. 规则 ----
all: compiler_test.exe cli.exe

# 编译：一个 .c 出一个 .o，全部放进 build/
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# PAL.c 用 #include 把平台实现文件直接包含进来（见 PAL.c 的分发逻辑），
# 而 make 默认只看 .c 文件，不知道这些实现文件也是 PAL.o 的依赖 ——
# 改了 WIN_std.c 不会触发 PAL.o 重建，会链接到过期的符号（undefined reference）。
# 这里显式补上依赖。
$(BUILD)/PAL.o: code/PAL/windows/WIN_std.c \
                code/PAL/windows/WIN_syscall.c \
                code/PAL/generic/PAL_generic.c \
                code/PAL/PAL.h

# build/ 不存在时才创建；-@ 表示失败也继续、不打印命令
$(BUILD):
	-@mkdir $(BUILD)

# 链接：把 .o 拼成 exe
compiler_test.exe: $(COMPILER_OBJ) $(KERNEL_OBJ)
	$(CC) $^ -o $@

# cli.c 自带 main()，不再需要 main.c；需同时链上编译器（不含 compiler_main）
cli.exe: $(CLI_OBJ) $(KERNEL_OBJ) $(COMPILER_LIB_OBJ)
	$(CC) $^ -o $@

clean:
	-@$(RM) -r $(BUILD)
	-@$(RM) compiler_test.exe cli.exe

.PHONY: all clean
