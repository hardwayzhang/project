CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE

BINS = server client client_recverr

.PHONY: all clean

all: $(BINS)

server: server.c common.h
	$(CC) $(CFLAGS) -o $@ server.c

client: client.c common.h
	$(CC) $(CFLAGS) -o $@ client.c

client_recverr: client_recverr.c common.h
	$(CC) $(CFLAGS) -o $@ client_recverr.c

clean:
	rm -f $(BINS)
