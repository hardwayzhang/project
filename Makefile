CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE

BINS = server client

.PHONY: all clean

all: $(BINS)

server: server.c common.h
	$(CC) $(CFLAGS) -o $@ server.c

client: client.c common.h
	$(CC) $(CFLAGS) -o $@ client.c

clean:
	rm -f $(BINS)
