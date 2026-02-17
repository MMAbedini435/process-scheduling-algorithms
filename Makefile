# Makefile for sched_ext_loadtest

CC      := gcc
CFLAGS  := -O2 -std=gnu11 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS :=

TARGET  := bin/loadtest
SRC     := loadtest.c

# Command to run before targets
SCX_CMD := scx_fifo

# Default runtime parameters (can override on command line)
MAX_PROCS ?= 20
SEED      ?= 2
CPU       ?= 0
LOG       ?= log/out.csv
TOTAL_LOG ?= log/runlog.csv
DELAY     ?= 10
MIN_ITERS ?= 1000000
MAX_ITERS ?= 5000000

.PHONY: all clean run debug

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

debug:
	$(CC) -O0 -g -std=gnu11 -Wall -Wextra -D_GNU_SOURCE $(SRC) -o $(TARGET)

run: $(TARGET)
	@echo "Starting scx_fifo..."
	@bash -c '\
		set -e; \
		sudo $(SCX_CMD) & \
		SCX_PID=$$!; \
		echo "scx_fifo PID: $$SCX_PID"; \
		sleep 2; \
		./$(TARGET) \
			-m $(MAX_PROCS) \
			-s $(SEED) \
			-c $(CPU) \
			-o $(LOG) \
			-d $(DELAY) \
			-w $(MIN_ITERS) \
			-W $(MAX_ITERS); \
		echo "Appending to total log...";\
		cat $(LOG) >> $(TOTAL_LOG); \
		echo "Target finished. Waiting 2 seconds..."; \
		sleep 2; \
		echo "Stopping scx_fifo..."; \
		kill -INT $$SCX_PID; \
		wait $$SCX_PID || true; \
	'
	@echo "Generating plots..."
	@set -e; \
	ts=$$(date +%Y%m%d_%H%M%S); \
	python3 plot.py --input $(LOG) --output "plots/$${ts}_1D.png" --no-gui --mode 1d; \
	python3 plot.py --input $(LOG) --output "plots/$${ts}_2D.png" --no-gui --mode 2d; \

clean:
	rm -f $(TARGET)