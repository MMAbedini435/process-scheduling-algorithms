/*
 * sched_ext_loadtest.c
 *
 * Build:
 *   gcc -O2 -std=gnu11 -Wall -Wextra -o sched_ext_loadtest sched_ext_loadtest.c
 *
 * Example run:
 *   ./sched_ext_loadtest -m 30 -s 12345 -c 0 -o runlog.csv
 *
 * The program writes CSV lines to the log file (append mode):
 * pid,child_index,start_ns,end_ns,duration_ns,work_iters
 *
 * Notes:
 * - Requires a kernel with sched_ext support to actually use the sched_ext scheduler.
 * - If SCHED_EXT is not available in your headers, we fall back to defining it as 7
 *   (value used by scx userland examples).
 *
 * Author: ChatGPT
 */
#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>

#ifndef SCHED_EXT
    #define SCHED_EXT 7
#endif

static inline uint64_t timespec_to_ns(const struct timespec *ts) {
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

/* Busy work function that cannot be optimized away */
static void do_busy_work(uint64_t iters) {
    volatile uint64_t sink = 0;
    for (uint64_t i = 0; i < iters; ++i) {
        /* some cheap ops to burn CPU without system calls */
        sink += (i ^ (sink << 1));
        /* prevent the compiler from optimizing this whole loop out */
        if ((i & 0x7ffff) == 0) asm volatile("" ::: "memory");
    }
    /* use sink in a way the compiler cannot remove */
    asm volatile("" : : "r"(sink) : "memory");
}

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
    /* Configurable parameters with reasonable defaults */
    int max_procs = 20;
    unsigned int seed = (unsigned int)time(NULL);
    int cpu_core = 0;
    const char *log_path = "sched_ext_runlog.csv";
    int max_start_delay_ms = 2000; /* max random delay before starting a child */
    uint64_t min_work_iters = 1000000ULL;
    uint64_t max_work_iters = 5000000ULL;
    // min_work_iters = 0ULL;
    // max_work_iters = 100000ULL;
    
    int opt;
    while ((opt = getopt(argc, argv, "m:s:c:o:d:w:W:")) != -1) {
        switch (opt) {
            case 'm': max_procs = atoi(optarg); break;
            case 's': seed = (unsigned int)strtoul(optarg, NULL, 10); break;
            case 'c': cpu_core = atoi(optarg); break;
            case 'o': log_path = optarg; break;
            case 'd': max_start_delay_ms = atoi(optarg); break;
            case 'w': min_work_iters = strtoull(optarg, NULL, 10); break;
            case 'W': max_work_iters = strtoull(optarg, NULL, 10); break;
            default:
            fprintf(stderr, "Usage: %s [-m max_procs] [-s seed] [-c cpu_core] [-o logfile] [-d max_start_delay_ms] [-w min_iters] [-W max_iters]\n", argv[0]);
            return 1;
        }
    }
    
    if (max_procs < 1) max_procs = 1;
    if (min_work_iters == 0) min_work_iters = 1;
    if (max_work_iters < min_work_iters) max_work_iters = min_work_iters;
    
    /* open log file (append) -- child processes inherit this FD */
    int logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logfd < 0) die("open(%s): %s\n", log_path, strerror(errno));
    
    /* write CSV header (only once per run) */
    {
        char header[] = "pid,child_index,start_ns,end_ns,duration_ns,work_iters\n";
        if (write(logfd, header, sizeof(header)-1) < 0) {
            /* not fatal; continue */
        }
    }
    
    /* deterministic RNG */
    srand(seed);
    
    /* random number of processes between 1..max_procs */
    int nprocs = 1 + (rand() % max_procs);
    printf("Seed=%u, creating %d child processes, cpu_core=%d\n", seed, nprocs, cpu_core);
    
    pid_t *children = calloc(nprocs, sizeof(pid_t));
    if (!children) die("calloc failed\n");

    struct timespec ts_begin;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_begin) != 0) {
        dprintf(logfd, "ERR: pid=%d clock_gettime start failed: %s\n", getpid(), strerror(errno));
        _exit(1);
    }
    uint64_t begin_ns = timespec_to_ns(&ts_begin);
    
    for (int i = 0; i < nprocs; ++i) {
        /* random delay before starting this child (so children start at random times) */
        int delay_ms = (max_start_delay_ms > 0) ? (rand() % (max_start_delay_ms + 1)) : 0;
        if (delay_ms > 0) {
            usleep((useconds_t)delay_ms * 1000);
        }

        pid_t pid = fork();
        if (pid < 0) {
            die("fork failed: %s\n", strerror(errno));
        } else if (pid == 0) {
            /* child */

            /* set CPU affinity to the chosen core */
            
            /* set scheduling policy to SCHED_EXT (if supported) */
            struct sched_param sp;
            sp.sched_priority = 0; /* sched_ext uses its own semantics (priority ignored here) */
            if (sched_setscheduler(0, SCHED_EXT, &sp) != 0) {
                /* Not fatal — if kernel doesn't have SCHED_EXT this will fail.
                * We log the error and proceed; scheduling will remain normal (CFS).
                */
               dprintf(logfd, "WARN: pid=%d sched_setscheduler(SCHED_EXT) failed: %s\n", getpid(), strerror(errno));
            }
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_core, &cpuset);
            if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
                /* affinity failure is non-fatal; we continue but warn */
                dprintf(logfd, "WARN: pid=%d failed to set affinity to cpu %d: %s\n", getpid(), cpu_core, strerror(errno));
            }
            
            /* compute work iterations (random) */
            /* Use rand() inherited from parent; fork copies RNG state so deterministic */
            uint64_t work_iters = min_work_iters;
            if (max_work_iters > min_work_iters) {
                work_iters = min_work_iters + (uint64_t)(rand() % (1 + (int)(max_work_iters - min_work_iters)));
            }

            /* Memory to store timestamps (captured while the process is actually running) */
            struct timespec ts_start, ts_end;

            /* IMPORTANT:
             * The first clock_gettime() and the following busy-loop happen while the
             * process is actually running on CPU (i.e., the measurement marks the time
             * when the process first executes the busy loop). We avoid syscalls
             * in between to prevent voluntary context switches during measured interval.
             */
            if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start) != 0) {
                dprintf(logfd, "ERR: pid=%d clock_gettime start failed: %s\n", getpid(), strerror(errno));
                _exit(1);
            }

            /* Busy work: never perform syscalls or sleeps while measuring */
            do_busy_work(work_iters);

            if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_end) != 0) {
                dprintf(logfd, "ERR: pid=%d clock_gettime end failed: %s\n", getpid(), strerror(errno));
                _exit(1);
            }

            uint64_t start_ns = timespec_to_ns(&ts_start);
            uint64_t end_ns   = timespec_to_ns(&ts_end);
            uint64_t dur_ns   = (end_ns >= start_ns) ? (end_ns - start_ns) : 0;

            /* Format the CSV line in a stack buffer and write via single write() call
             * to avoid interleaving between processes (atomic for small writes).
             * The write happens *after* measurements, so it does not change timing.
             */
            char buf[256];
            int len = snprintf(buf, sizeof(buf), "%d,%d,%llu,%llu,%llu,%llu\n",
                    (int)getpid(), i,
                    (unsigned long long)start_ns - begin_ns,
                    (unsigned long long)end_ns - begin_ns,
                    (unsigned long long)dur_ns,
                    (unsigned long long)work_iters);
            if (len > 0) {
                /* Single write call is safer from an atomicity perspective */
                ssize_t w = write(logfd, buf, (size_t)len);
                (void)w;
            }

            _exit(0);
        } else {
            /* parent */
            children[i] = pid;
        }
    }

    /* parent waits for all children */
    for (int i = 0; i < nprocs; ++i) {
        if (children[i] > 0) {
            int status = 0;
            waitpid(children[i], &status, 0);
        }
    }

    printf("All children finished, log appended to %s\n", log_path);
    // print pids
    printf("Child PIDs in order:\n");
    for (int i = 0; i < nprocs; i++)
    {
        printf("\t%d\n", children[i]);
    }
    
    close(logfd);
    free(children);
    return 0;
}