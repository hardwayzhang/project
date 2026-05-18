# Build the fast-backtrace benchmark.
#
# IMPORTANT: -fno-omit-frame-pointer is mandatory for the FP-based
# unwinder to work. -rdynamic exports symbols so backtrace_symbols()
# can name them. -O2 is on so the comparison reflects realistic
# release-build performance; -fasynchronous-unwind-tables is the gcc
# default and provides .eh_frame for glibc backtrace().

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -pthread \
           -fno-omit-frame-pointer \
           -fasynchronous-unwind-tables
LDFLAGS ?= -rdynamic -pthread

SRC := src/fast_backtrace.c src/bench.c
OBJ := $(SRC:.c=.o)
BIN := bench_backtrace

# Counter-example: same source built WITHOUT frame pointers. The FP-based
# unwinder will return depth=0 or a truncated stack, demonstrating why
# -fno-omit-frame-pointer is mandatory for this technique.
NOFP_OBJ := $(SRC:.c=.nofp.o)
NOFP_BIN := bench_backtrace_nofp
NOFP_CFLAGS := -O2 -g -Wall -Wextra -pthread -fomit-frame-pointer \
               -fasynchronous-unwind-tables

.PHONY: all clean run run-nofp

all: $(BIN) $(NOFP_BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

$(NOFP_BIN): $(NOFP_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

%.nofp.o: %.c
	$(CC) $(NOFP_CFLAGS) -Isrc -c -o $@ $<

run: $(BIN)
	./$(BIN)

run-nofp: $(NOFP_BIN)
	./$(NOFP_BIN)

clean:
	rm -f $(OBJ) $(NOFP_OBJ) $(BIN) $(NOFP_BIN)
