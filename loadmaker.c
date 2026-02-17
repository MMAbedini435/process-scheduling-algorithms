#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sched.h>

#ifndef SCHED_EXT
    #define SCHED_EXT 7
#endif

/* ---------- config ---------- */
typedef struct {
    int seed;
    int max_procs;
    int max_start_delay_ms;
    int max_runtime_ms;
    char logfile[256];
} config_t;

static int64_t program_start_us;

/* -------------------------------------------------- */
/* Utility: current time in microseconds              */
/* -------------------------------------------------- */
static int64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/* -------------------------------------------------- */
/* Safe write: ensure all bytes written or return -1  */
/* -------------------------------------------------- */
static ssize_t write_all(int fd, const void *buf, size_t count) {
    size_t written = 0;
    const char *p = (const char *)buf;
    while (written < count) {
        ssize_t w = write(fd, p + written, count - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += (size_t)w;
    }
    return (ssize_t)written;
}

/* -------------------------------------------------- */
/* CPU-bound workload with first-run detection and   */
/* CPU-time measurement (thread CPU time)             */
/* Inputs:
 *  - duration_ms: requested wall-clock duration
 *  - out_first_run_us: pointer (in microseconds since epoch) set at first execution moment (0 if never)
 *  - out_end_us: wall-clock end timestamp
 *  - out_cpu_ms: CPU time consumed in milliseconds
 */
/* -------------------------------------------------- */
static void busy_work_measure(long duration_ms,
                              int64_t *out_first_run_us,
                              int64_t *out_end_us,
                              long *out_cpu_ms) {

    if (out_first_run_us) *out_first_run_us = 0;
    if (out_end_us) *out_end_us = 0;
    if (out_cpu_ms) *out_cpu_ms = 0;

    struct timespec cpu_start, cpu_end;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start) != 0) {
        /* best-effort: continue even if this fails */
        cpu_start.tv_sec = cpu_start.tv_nsec = 0;
    }

    int64_t start_wall = now_us();
    int64_t target_wall = start_wall + duration_ms * 1000LL;

    /* Busy loop; record the first time we actually run (first iteration) */
    while (now_us() < target_wall) {
        if (out_first_run_us && *out_first_run_us == 0) {
            *out_first_run_us = now_us();
        }
        /* prevent compiler optimizing loop away */
        asm volatile("" ::: "memory");
    }

    int64_t end_wall = now_us();

    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end) == 0) {
        long cpu_ns = (long)((cpu_end.tv_sec - cpu_start.tv_sec) * 1000000000LL
                      + (cpu_end.tv_nsec - cpu_start.tv_nsec));
        if (out_cpu_ms) *out_cpu_ms = cpu_ns / 1000000L;
    }

    if (out_end_us) *out_end_us = end_wall;
}

/* -------------------------------------------------- */
/* Child execution logic                               */
/* -------------------------------------------------- */
static void run_child(int start_delay_ms, int runtime_ms, const char *logfile) {

    /* Sleep until arrival */
    if (start_delay_ms > 0) {
        usleep((useconds_t)start_delay_ms * 1000);
    }

    /* Arrival = moment we became runnable (end of sleep) */
    int64_t arrival_us = now_us();
    int64_t arrival_rel_ms = (arrival_us - program_start_us) / 1000LL;

    /* Prepare measurement placeholders */
    int64_t first_run_us = 0;
    int64_t end_wall_us = 0;
    long cpu_time_ms = 0;

    /* Do busy work; first_run_us will be set on first actual CPU execution */
    busy_work_measure(runtime_ms, &first_run_us, &end_wall_us, &cpu_time_ms);

    /* If for some reason first_run_us not set (duration==0), set it to arrival_us */
    if (first_run_us == 0) first_run_us = arrival_us;

    int64_t start_rel_ms = (first_run_us - program_start_us) / 1000LL;
    int64_t end_rel_ms = (end_wall_us - program_start_us) / 1000LL;

    long wait_ms = (long)(start_rel_ms - arrival_rel_ms);
    if (wait_ms < 0) wait_ms = 0; /* safety */

    long run_wall_ms = (long)(end_rel_ms - start_rel_ms);
    if (run_wall_ms < 0) run_wall_ms = 0;

    /* Open logfile and append */
    int fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char buffer[512];
        int len = snprintf(buffer, sizeof(buffer),
            "PID=%d ARRIVAL_MS=%lld START_MS=%lld END_MS=%lld WAIT_MS=%ld RUN_WALL_MS=%ld RUN_CPU_MS=%ld\n",
            (int)getpid(),
            (long long)arrival_rel_ms,
            (long long)start_rel_ms,
            (long long)end_rel_ms,
            wait_ms,
            run_wall_ms,
            (long)cpu_time_ms);

        if (len > 0) {
            if (write_all(fd, buffer, (size_t)len) < 0) {
                /* best-effort: print to stderr if cannot log */
                perror("write_all");
            }
        }
        close(fd);
    } else {
        perror("open logfile (child)");
    }

    _exit(0);
}

/* -------------------------------------------------- */
/* Argument parsing                                    */
/* -------------------------------------------------- */
static int parse_args(int argc, char *argv[], config_t *cfg) {

    if (argc != 6) {
        fprintf(stderr,
            "Usage: %s <seed> <max_procs> <max_start_delay_ms> <max_runtime_ms> <logfile>\n",
            argv[0]);
        return -1;
    }

    cfg->seed = atoi(argv[1]);
    cfg->max_procs = atoi(argv[2]);
    cfg->max_start_delay_ms = atoi(argv[3]);
    cfg->max_runtime_ms = atoi(argv[4]);

    if (cfg->max_procs < 1) cfg->max_procs = 1;
    if (cfg->max_start_delay_ms < 0) cfg->max_start_delay_ms = 0;
    if (cfg->max_runtime_ms < 1) cfg->max_runtime_ms = 1;

    strncpy(cfg->logfile, argv[5], sizeof(cfg->logfile) - 1);
    cfg->logfile[sizeof(cfg->logfile) - 1] = '\0';

    return 0;
}

/* -------------------------------------------------- */
/* Main                                                */
/* -------------------------------------------------- */
int main(int argc, char *argv[]) {

    config_t cfg;
    if (parse_args(argc, argv, &cfg) != 0)
        return 1;

    /* Seed RNG */
    srand(cfg.seed);

    /* Reset log file at start (truncate) */
    int fd = open(cfg.logfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("logfile open");
        return 1;
    }
    close(fd);

    /* program start reference */
    program_start_us = now_us();

    /* number of processes: 1 .. max_procs */
    int num_procs = (cfg.max_procs > 1) ? (rand() % cfg.max_procs) + 1 : 1;
    printf("Generating %d processes\n", num_procs);

    for (int i = 0; i < num_procs; i++) {

        int start_delay = 0;
        if (cfg.max_start_delay_ms > 0)
            start_delay = rand() % cfg.max_start_delay_ms;

        int runtime = 1;
        if (cfg.max_runtime_ms > 0)
            runtime = (rand() % cfg.max_runtime_ms) + 1;

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            /* continue trying to create remaining children */
            continue;
        }
        if (pid == 0) {
            /* child */
            // set to sched-ext class
            struct sched_param param;
            param.sched_priority = 0;
            if (sched_setscheduler(0, SCHED_EXT, &param) == -1) {
                perror("sched_setscheduler");
                exit(EXIT_FAILURE);
            }
            cpu_set_t set;

            CPU_ZERO(&set);      // Clear the set
            CPU_SET(0, &set);    // Add CPU 0

            if (sched_setaffinity(0, sizeof(set), &set) == -1) {
                perror("sched_setaffinity");
                exit(EXIT_FAILURE);
            }
            run_child(start_delay, runtime, cfg.logfile);
            /* run_child does _exit() */
        }
        /* parent continues loop */
    }

    /* parent waits for children */
    for (int i = 0; i < num_procs; i++) {
        wait(NULL);
    }

    printf("All processes finished.\n");
    return 0;
}

// taskset -c 0 ./loadgen 1234 10 1 3000 log.txt


