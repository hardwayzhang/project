# Makefile for crc_hwaccel_test (x86 only).
#
# IMPORTANT: we deliberately do NOT pass -msse4.2 / -mpclmul / -mavx etc.
# globally. The whole project is compiled at the baseline x86 ISA, and only
# the single function `crc32c_hw` (in crc_hw.c) opts in to SSE4.2 via
# __attribute__((target("sse4.2"))). This way the rest of the program stays
# portable across older x86 CPUs and the build flags aren't "polluted" with
# instruction-set switches.
#
# Targets:
#   make          - build the test program for the host x86 platform
#   make run      - build and run the detection + self-test
#   make bench    - build and run the throughput benchmark
#   make clean    - remove build artifacts

CC      ?= cc
CSTD    ?= -std=c11
WARN    ?= -Wall -Wextra -Wpedantic
OPT     ?= -O2
CFLAGS  ?= $(CSTD) $(WARN) $(OPT)
LDFLAGS ?=

UNAME_M := $(shell uname -m)

ifeq ($(filter $(UNAME_M),x86_64 i386 i686),)
  $(error This project targets x86 / x86_64 only (host is $(UNAME_M)))
endif

TARGET  := crc_hwaccel_test
OBJS    := crc_hwaccel_test.o crc_hw.o
HEADERS := crc_hw.h

.PHONY: all run bench clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

bench: $(TARGET)
	./$(TARGET) --bench

clean:
	rm -f $(TARGET) $(OBJS)
