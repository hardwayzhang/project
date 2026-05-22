CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=

TARGET  := disk_write_test
SRC     := disk_write_test.c

.PHONY: all clean run run-small

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

# A quick smoke run that uses a small data size so it finishes fast.
run-small: $(TARGET)
	./$(TARGET) -s 64

# A more meaningful run that writes 512 MiB per mode/block.
run: $(TARGET)
	./$(TARGET) -s 512

clean:
	rm -f $(TARGET) disk_write_test.dat
