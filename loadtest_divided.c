/*
 * sched_ext_loadtest.c
 *
 * Build:
 *   gcc -O2 -std=gnu11 -Wall -Wextra -o sched_ext_loadtest sched_ext_loadtest.c
 *
 * Example run:
 *   ./sched_ext_loadtest -m 30 -s 12345 -c 0 -o runlog.csv -u 10000
 *
 * The program writes CSV lines to the log file (replace mode):
 * pid,child_index,arrive_ns,start_ns,end_ns,duration_ns,work_iters
 *
 * Each child divides its total work_iters into units of size 'unit_iters' and
 * logs start/end/duration for each unit — all lines for a child are written
 * with a single write() call at child exit to avoid syscalls between slices.
 *
 * Author: ChatGPT (modified)
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
    uint64_t unit_iters = 10000ULL; /* iterations per measured slice */

    int opt;
    while ((opt = getopt(argc, argv, "m:s:c:o:d:w:W:u:")) != -1) {
        switch (opt) {
            case 'm': max_procs = atoi(optarg); break;
            case 's': seed = (unsigned int)strtoul(optarg, NULL, 10); break;
            case 'c': cpu_core = atoi(optarg); break;
            case 'o': log_path = optarg; break;
            case 'd': max_start_delay_ms = atoi(optarg); break;
            case 'w': min_work_iters = strtoull(optarg, NULL, 10); break;
            case 'W': max_work_iters = strtoull(optarg, NULL, 10); break;
            case 'u': unit_iters = strtoull(optarg, NULL, 10); break;
            default:
            fprintf(stderr, "Usage: %s [-m max_procs] [-s seed] [-c cpu_core] [-o logfile] [-d max_start_delay_ms] [-w min_iters] [-W max_iters] [-u unit_iters]\n", argv[0]);
            return 1;
        }
    }
    
    if (max_procs < 1) max_procs = 1;
    if (min_work_iters == 0) min_work_iters = 1;
    if (max_work_iters < min_work_iters) max_work_iters = min_work_iters;
    if (unit_iters == 0) unit_iters = 1;

    /* open log file (replace) -- child processes inherit this FD */
    int logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (logfd < 0) die("open(%s): %s\n", log_path, strerror(errno));
    
    /* write CSV header (only once per run) */
    {
        char header[] = "pid,child_index,arrive_ns,start_ns,end_ns,duration_ns,work_iters\n";
        if (write(logfd, header, sizeof(header)-1) < 0) {
            /* not fatal; continue */
        }
    }
    
    /* deterministic RNG */
    srand(seed);
    
    /* random number of processes between 1..max_procs */
    int nprocs = 1 + (rand() % max_procs);
    printf("Seed=%u, creating %d child processes, cpu_core=%d, unit_iters=%llu\n", seed, nprocs, cpu_core, (unsigned long long)unit_iters);
    
    pid_t *children = calloc(nprocs, sizeof(pid_t));
    if (!children) die("calloc failed\n");
    
    struct timespec ts_begin;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_begin) != 0) {
        dprintf(logfd, "ERR: pid=%d clock_gettime start failed: %s\n", getpid(), strerror(errno));
        _exit(1);
    }
    uint64_t begin_ns = timespec_to_ns(&ts_begin);
    
    for (int i = 0; i < nprocs; ++i) {
        
        struct timespec ts_arrive;
        if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_arrive)!= 0) {
            dprintf(logfd, "ERR: pid=%d clock_gettime start failed: %s\n", getpid(), strerror(errno));
            _exit(1);
        } 
        /* random delay (so children start at random times) */
        int delay_ms = (max_start_delay_ms > 0) ? (rand() % (max_start_delay_ms + 1)) : 0;
        uint64_t arrive_ns = timespec_to_ns(&ts_arrive) + delay_ms * 1000000ULL;
        if (delay_ms > 0) {
            usleep((useconds_t)delay_ms * 1000);
        }
        pid_t pid = fork();
        if (pid < 0) {
            die("fork failed: %s\n", strerror(errno));
        } else if (pid == 0) {
            /* child */
            
            /* set scheduling policy to SCHED_EXT (if supported) */
            struct sched_param sp;
            sp.sched_priority = 0; /* sched_ext uses its own semantics (priority ignored here) */
            if (sched_setscheduler(0, SCHED_EXT, &sp) != 0) {
                /* Not fatal — if kernel doesn't have SCHED_EXT this will fail.
                * We log the error and proceed; scheduling will remain normal (CFS).
                * These logs happen before measurement loop.
                */
               dprintf(logfd, "WARN: pid=%d sched_setscheduler(SCHED_EXT) failed: %s\n", getpid(), strerror(errno));
            }
            /* set CPU affinity to the chosen core */
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

            /* number of slices */
            uint64_t slices = (work_iters + unit_iters - 1) / unit_iters;
            if (slices == 0) slices = 1;

            /* allocate arrays to store timestamps for each slice */
            uint64_t *start_ns_arr = calloc((size_t)slices, sizeof(uint64_t));
            uint64_t *end_ns_arr   = calloc((size_t)slices, sizeof(uint64_t));
            if (!start_ns_arr || !end_ns_arr) {
                dprintf(logfd, "ERR: pid=%d failed to allocate timestamp arrays\n", getpid());
                _exit(1);
            }

            /* perform measured slices, storing timestamps (no logging during loop) */
            struct timespec ts_start, ts_end;
            uint64_t remaining = work_iters;
            uint64_t idx = 0;
            while (remaining > 0 && idx < slices) {
                uint64_t cur = (remaining > unit_iters) ? unit_iters : remaining;

                if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start) != 0) {
                    dprintf(logfd, "ERR: pid=%d clock_gettime start failed: %s\n", getpid(), strerror(errno));
                    _exit(1);
                }

                /* Busy work for this slice (no syscalls while measuring) */
                do_busy_work(cur);

                if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_end) != 0) {
                    dprintf(logfd, "ERR: pid=%d clock_gettime end failed: %s\n", getpid(), strerror(errno));
                    _exit(1);
                }

                start_ns_arr[idx] = timespec_to_ns(&ts_start);
                end_ns_arr[idx]   = timespec_to_ns(&ts_end);

                remaining -= cur;
                ++idx;
            }

            /* Format all slice CSV lines into a single buffer, then write once. */
            size_t estimated_per_line = 120;
            size_t buflen = (size_t)slices * estimated_per_line + 256;
            char *buf = malloc(buflen);
            if (!buf) {
                dprintf(logfd, "ERR: pid=%d failed to allocate output buffer\n", getpid());
                free(start_ns_arr);
                free(end_ns_arr);
                _exit(1);
            }
            size_t off = 0;
            for (uint64_t k = 0; k < idx; ++k) {
                uint64_t slice_work = (k + 1 < slices) ? unit_iters : (work_iters - unit_iters * (slices - 1));
                /* handle case when computed slice_work becomes 0 (shouldn't happen) */
                if (slice_work == 0) slice_work = unit_iters;

                uint64_t s_ns = start_ns_arr[k];
                uint64_t e_ns = end_ns_arr[k];
                uint64_t d_ns = (e_ns >= s_ns) ? (e_ns - s_ns) : 0;

                /* ensure buffer large enough, resize if necessary */
                int need = 0;
                /* estimate remaining required — snprintf will tell exact len; check and grow if it doesn't fit */
                int len = snprintf(buf + off, (off < buflen) ? (buflen - off) : 0,
                        "%d,%d,%llu,%llu,%llu,%llu,%llu\n",
                        (int)getpid(), i,
                        (unsigned long long)(arrive_ns - begin_ns),
                        (unsigned long long)(s_ns - begin_ns),
                        (unsigned long long)(e_ns - begin_ns),
                        (unsigned long long)d_ns,
                        (unsigned long long)slice_work);
                if (len < 0) {
                    /* snprintf error */
                    free(buf);
                    free(start_ns_arr);
                    free(end_ns_arr);
                    dprintf(logfd, "ERR: pid=%d snprintf failed\n", getpid());
                    _exit(1);
                }
                if ((size_t)len >= (buflen - off)) {
                    /* need to grow buffer and retry this line */
                    size_t new_buflen = buflen * 2 + (size_t)len + 256;
                    char *n = realloc(buf, new_buflen);
                    if (!n) {
                        free(buf);
                        free(start_ns_arr);
                        free(end_ns_arr);
                        dprintf(logfd, "ERR: pid=%d realloc failed\n", getpid());
                        _exit(1);
                    }
                    buf = n;
                    buflen = new_buflen;
                    /* write again into newly-sized buffer */
                    len = snprintf(buf + off, buflen - off,
                        "%d,%d,%llu,%llu,%llu,%llu,%llu\n",
                        (int)getpid(), i,
                        (unsigned long long)(arrive_ns - begin_ns),
                        (unsigned long long)(s_ns - begin_ns),
                        (unsigned long long)(e_ns - begin_ns),
                        (unsigned long long)d_ns,
                        (unsigned long long)slice_work);
                    if (len < 0 || (size_t)len >= (buflen - off)) {
                        free(buf);
                        free(start_ns_arr);
                        free(end_ns_arr);
                        dprintf(logfd, "ERR: pid=%d snprintf retry failed\n", getpid());
                        _exit(1);
                    }
                }
                off += (size_t)len;
            }

            /* single write of all slice lines */
            if (off > 0) {
                ssize_t w = write(logfd, buf, off);
                (void)w;
            }

            free(buf);
            free(start_ns_arr);
            free(end_ns_arr);

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