# Makefile for sched_ext_loadtest

CC      := gcc
CFLAGS  := -O2 -std=gnu11 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS :=

TARGET  := loadtest
SRC     := loadtest.c

# Default runtime parameters (can override on command line)
MAX_PROCS ?= 20
SEED      ?= 12345
CPU       ?= 0
LOG       ?= runlog.csv
DELAY     ?= 2000
MIN_ITERS ?= 1000000
MAX_ITERS ?= 5000000

.PHONY: all clean run sudo-run debug

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

debug:
	$(CC) -O0 -g -std=gnu11 -Wall -Wextra -D_GNU_SOURCE $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET) \
		-m $(MAX_PROCS) \
		-s $(SEED) \
		-c $(CPU) \
		-o $(LOG) \
		-d $(DELAY) \
		-w $(MIN_ITERS) \
		-W $(MAX_ITERS)

# Use this if sched_setscheduler requires CAP_SYS_NICE
sudo-run: $(TARGET)
	sudo ./$(TARGET) \
		-m $(MAX_PROCS) \
		-s $(SEED) \
		-c $(CPU) \
		-o $(LOG) \
		-d $(DELAY) \
		-w $(MIN_ITERS) \
		-W $(MAX_ITERS)

clean:
	rm -f $(TARGET)