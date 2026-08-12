# Makefile - COW do_wp_page 观测 Demo
#
# 目标:
#   make            编译 cow_demo
#   make clean      清理
#   make check      运行环境检测 (setup.sh)
#
# 说明: 采用 -fno-omit-frame-pointer 以便 perf 能基于帧指针回溯用户态调用栈;
#       -g 保留符号, 便于符号化子进程用户态堆栈。

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -fno-omit-frame-pointer
LDFLAGS ?=

SRCDIR  := src
BIN     := cow_demo
OBJS    := $(SRCDIR)/cow_demo.o $(SRCDIR)/symbolize.o

.PHONY: all clean check

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/symbolize.h
	$(CC) $(CFLAGS) -c -o $@ $<

check:
	@sh ./setup.sh

clean:
	rm -f $(BIN) $(OBJS)
