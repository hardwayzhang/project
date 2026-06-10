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
CC      := gcc
CFLAGS  ?= -fsanitize=address -fno-omit-frame-pointer -g -O1 -Wall -Wextra
SRC_DIR := src
BIN_DIR := bin

SRCS := $(wildcard $(SRC_DIR)/*.c)
BINS := $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/%,$(SRCS))

.PHONY: all run clean

all: $(BINS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/%: $(SRC_DIR)/%.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $<

run: all
	@./run_demo.sh

clean:
	rm -rf $(BIN_DIR)
