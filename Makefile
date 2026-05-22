# Makefile for crc_hwaccel_test
#
# `make`           - build the test program for the current platform.
# `make run`       - build and run feature detection + self-test.
# `make bench`     - build and run the throughput benchmark.
# `make clean`     - remove build artifacts.

CC      ?= cc
CSTD    ?= -std=c11
WARN    ?= -Wall -Wextra -Wpedantic
OPT     ?= -O2
CFLAGS  ?= $(CSTD) $(WARN) $(OPT)
LDFLAGS ?=

UNAME_M := $(shell uname -m)

# Enable the proper instruction sets per architecture so that target-attributed
# functions (e.g. SSE4.2, +crc) actually get the intrinsics' inline asm.
ifeq ($(filter $(UNAME_M),x86_64 i386 i686),$(UNAME_M))
  ARCH_CFLAGS := -msse4.2 -mpclmul
else ifeq ($(UNAME_M),aarch64)
  ARCH_CFLAGS := -march=armv8-a+crc+crypto
else ifneq (,$(findstring arm,$(UNAME_M)))
  ARCH_CFLAGS := -march=armv8-a+crc
else
  ARCH_CFLAGS :=
endif

TARGET  := crc_hwaccel_test
SRC     := crc_hwaccel_test.c

.PHONY: all run bench clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

bench: $(TARGET)
	./$(TARGET) --bench

clean:
	rm -f $(TARGET)
