# Makefile for sched_ext_loadtest

CC      := gcc
CFLAGS  := -O2 -std=gnu11 -Wall -Wextra -D_GNU_SOURCE
STAT_FLAGS	:= -O2 -Wall
LDFLAGS :=
BIN_DIR := bin
CAPTURE_CMD := :

TARGET   := $(BIN_DIR)/loadtest
SRC      := loadtest.c
PLOTTER  := plot.py

STAT_SRC := scx_fifo_stats.c
STAT_BIN := $(BIN_DIR)/scx_fifo_stats

# Default runtime parameters (can override on command line)
MAX_PROCS ?= 20
SEED      ?= 2
CPU       ?= 0
LOG       ?= log/out.csv
TOTAL_LOG ?= log/runlog.csv
DELAY     ?= 10
MIN_ITERS ?= 1000000
MAX_ITERS ?= 5000000

.PHONY: all clean run run_fifo run_mlfq run_capture debug

########################################
# Build
########################################

all: $(TARGET) $(STAT_BIN)
build_stat: $(STAT_BIN)

$(TARGET): $(SRC)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(STAT_BIN): $(STAT_SRC)
	@mkdir -p $(BIN_DIR)
	$(CC) $(STAT_FLAGS) $< -o $@ -lbpf

debug:
	@mkdir -p $(BIN_DIR)
	$(CC) -O0 -g -std=gnu11 -Wall -Wextra -D_GNU_SOURCE $(SRC) -o $(TARGET)

########################################
# FIFO run target
########################################
run_fifo: SCX_CMD=scx_fifo
run_fifo: run

########################################
# MLFQ run target
########################################
run_mlfq: SCX_CMD=scx_mlfq
run_mlfq: MIN_ITERS=8000000
run_mlfq: MAX_ITERS=40000000
run_mlfq: DELAY=200
run_mlfq: PLOTTER=plot_micro.py
run_mlfq: run

########################################
# Capture run target
########################################
run_capture: SCX_CMD=scx_fifo_capture
run_capture: CAPTURE_CMD=sudo $(STAT_BIN)
run_capture: run

########################################
# Shared run logic
########################################
run: $(TARGET) $(STAT_BIN)
	@echo "Starting $(SCX_CMD)..."
	@bash -c '\
		set -e; \
		sudo $(SCX_CMD) & \
		SCX_PID=$$!; \
		echo "$(SCX_CMD) PID: $$SCX_PID"; \
		sleep 2; \
		./$(TARGET) \
			-m $(MAX_PROCS) \
			-s $(SEED) \
			-c $(CPU) \
			-o $(LOG) \
			-d $(DELAY) \
			-w $(MIN_ITERS) \
			-W $(MAX_ITERS); \
		echo "Appending to total log..."; \
		cat $(LOG) >> $(TOTAL_LOG); \
		echo "Target finished. Waiting 2 seconds..."; \
		$(CAPTURE_CMD); \
		sleep 2; \
		echo "Stopping $(SCX_CMD)..."; \
		kill -INT $$SCX_PID; \
		wait $$SCX_PID || true; \
	'
	@echo "Generating plots..."
	@set -e; \
	ts=$$(date +%Y%m%d_%H%M%S); \
	python3 $(PLOTTER) --input $(LOG) --output "plots/$${ts}_1D.png" --no-gui --mode 1d; \
	python3 $(PLOTTER) --input $(LOG) --output "plots/$${ts}_2D.png" --no-gui --mode 2d;

########################################
# Clean
########################################
clean:
	rm -f $(TARGET) $(STAT_BIN)