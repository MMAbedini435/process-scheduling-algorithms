/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Userspace loader for scx_mlfq.
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <libgen.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>

#include <bpf/bpf.h>
#include <scx/common.h>

#include "scx_mlfq.bpf.skel.h"

const char help_fmt[] =
"A 2-level MLFQ sched_ext scheduler.\n"
"\n"
"Policy:\n"
"  - Top: Round-Robin (default 50ms slice). All tasks start here.\n"
"  - After a task runs once in the top queue, it is demoted to bottom.\n"
"  - Bottom: FIFO.\n"
"\n"
"Usage: %s [-a] [-s RR_SLICE_MS] [-v]\n"
"\n"
"  -a            Schedule all eligible tasks (full mode). Default is partial\n"
"                mode (SCX_OPS_SWITCH_PARTIAL), which schedules only\n"
"                SCHED_EXT tasks.\n"
"  -s MS         Set top-queue RR time slice in milliseconds (default: 50).\n"
"  -v            Print libbpf debug messages\n"
"  -h            Display this help and exit\n";

static bool verbose;
static volatile int exit_req;

static int libbpf_print_fn(enum libbpf_print_level level,
			   const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sigint_handler(int dummy)
{
	exit_req = 1;
}

static void read_stats(struct scx_mlfq *skel, __u64 out[3])
{
	int nr_cpus = libbpf_num_possible_cpus();
	__u64 cnts[3][nr_cpus];
	__u32 idx;

	out[0] = out[1] = out[2] = 0;

	for (idx = 0; idx < 3; idx++) {
		int ret, cpu;

		ret = bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats),
					  &idx, cnts[idx]);
		if (ret < 0)
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			out[idx] += cnts[idx][cpu];
	}
}

static int parse_u64(const char *s, __u64 *out)
{
	char *end = NULL;
	unsigned long long v;

	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || !end || *end != '\0')
		return -EINVAL;
	*out = (__u64)v;
	return 0;
}

int main(int argc, char **argv)
{
	struct scx_mlfq *skel;
	struct bpf_link *link;
	__u32 opt;
	__u64 ecode;
	bool all_tasks = false;
	__u64 rr_ms = 50;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

restart:
	skel = SCX_OPS_OPEN(mlfq_ops, scx_mlfq);

	while ((opt = getopt(argc, argv, "as:vh")) != -1) {
		switch (opt) {
		case 'a':
			all_tasks = true;
			break;
		case 's':
			if (parse_u64(optarg, &rr_ms)) {
				fprintf(stderr, "Invalid -s value: %s\n", optarg);
				return 1;
			}
			break;
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	/* Set the RR slice (ns) for the top queue. */
	skel->rodata->rr_slice_ns = rr_ms * 1000ULL * 1000ULL;

	/* Enforce/adjust partial-switch mode per CLI. */
	if (all_tasks)
		skel->struct_ops.mlfq_ops->flags &= ~SCX_OPS_SWITCH_PARTIAL;
	else
		skel->struct_ops.mlfq_ops->flags |= SCX_OPS_SWITCH_PARTIAL;

	SCX_OPS_LOAD(skel, mlfq_ops, scx_mlfq, uei);
	link = SCX_OPS_ATTACH(skel, mlfq_ops, scx_mlfq);

	printf("scx_mlfq: rr_slice_ms=%llu mode=%s\n",
	       (unsigned long long)rr_ms,
	       all_tasks ? "full" : "partial");

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 st[3];

		read_stats(skel, st);
		printf("local=%llu rr=%llu fifo=%llu\n",
		       (unsigned long long)st[0],
		       (unsigned long long)st[1],
		       (unsigned long long)st[2]);
		fflush(stdout);
		sleep(1);
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_mlfq__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
