CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
WARNINGS := -Wall -Wextra -Wpedantic -Werror

.PHONY: all clean test

all: madvise_uss

madvise_uss: madvise_uss.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -o $@ $<

test: madvise_uss
	./madvise_uss
	./madvise_uss 17 5

clean:
	rm -f madvise_uss
