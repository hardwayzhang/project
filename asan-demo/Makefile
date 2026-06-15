# AddressSanitizer (ASan) 内存检测示例 Makefile
#
# 用法：
#   make            # 使用 ASan 编译所有示例
#   make CC=clang   # 改用 clang 编译
#   make run        # 编译并依次运行所有示例
#   make clean      # 清理生成物
#
# 关键编译选项：
#   -fsanitize=address      启用 AddressSanitizer
#   -fno-omit-frame-pointer 保留帧指针，让报错栈回溯更清晰
#   -g                      生成调试信息，报告里能显示文件名和行号
#   -O1                     轻度优化，ASan 推荐使用

# 默认使用 gcc（注意：make 的内置默认 CC=cc，故这里用 := 强制覆盖；
# 命令行 `make CC=clang` 仍可正常覆盖本设置）。
CC         := gcc
CFLAGS     ?= -fsanitize=address -fno-omit-frame-pointer -g -O1 -Wall -Wextra
SRC_DIR    := src
COMMON_DIR := common
BIN_DIR    := bin

# 公共源文件（如 __asan_default_options 的定义）会链接进每个示例，
# 这样运行时无需再设置 ASAN_OPTIONS 环境变量。
COMMON_SRCS := $(wildcard $(COMMON_DIR)/*.c)

# 允许示例 #include common/ 下的头文件。
CPPFLAGS += -I$(COMMON_DIR)

# fork_child_overflow 需要静态链接 ASan 运行时，才能引用内部的
# AsanActivate/AsanDeactivate 符号。gcc 用 -static-libasan；clang 默认即静态。
STATIC_ASAN_FLAG := $(if $(findstring clang,$(CC)),,-static-libasan)

SRCS := $(wildcard $(SRC_DIR)/*.c)
BINS := $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/%,$(SRCS))

.PHONY: all run clean

all: $(BINS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# fork_child_overflow：静态链接 asan，且自带 __asan_default_options，
# 故不链接 common/ 里的 asan_default_options.c（避免重复定义）。
$(BIN_DIR)/fork_child_overflow: $(SRC_DIR)/fork_child_overflow.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(STATIC_ASAN_FLAG) -o $@ $<

# fork_log_split：用 -fsanitize-recover=address 让报告后可继续，
# 自带 __asan_default_options，故不链接 common/。
$(BIN_DIR)/fork_log_split: $(SRC_DIR)/fork_log_split.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize-recover=address -o $@ $<

$(BIN_DIR)/%: $(SRC_DIR)/%.c $(COMMON_SRCS) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(COMMON_SRCS)

run: all
	@./run_demo.sh

clean:
	rm -rf $(BIN_DIR)
