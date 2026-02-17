/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Userspace loader for scx_fifo.
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <libgen.h>
#include <stdarg.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_fifo.bpf.skel.h"

const char help_fmt[] =
"A minimal global FIFO sched_ext scheduler.\n"
"\n"
"Usage: %s [-v]\n"
"\n"
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

static void read_stats(struct scx_fifo *skel, __u64 *out)
{
	int nr_cpus = libbpf_num_possible_cpus();
	__u64 cnts[2][nr_cpus];
	__u32 idx;

	out[0] = out[1] = 0;

	for (idx = 0; idx < 2; idx++) {
		int ret, cpu;

		ret = bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats),
					  &idx, cnts[idx]);
		if (ret < 0)
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			out[idx] += cnts[idx][cpu];
	}
}

int main(int argc, char **argv)
{
	struct scx_fifo *skel;
	struct bpf_link *link;
	__u32 opt;
	__u64 ecode;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	skel = SCX_OPS_OPEN(fifo_ops, scx_fifo);
	skel->struct_ops.fifo_ops->flags |= SCX_OPS_SWITCH_PARTIAL;
	while ((opt = getopt(argc, argv, "vh")) != -1) {
		switch (opt) {
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	SCX_OPS_LOAD(skel, fifo_ops, scx_fifo, uei);
	link = SCX_OPS_ATTACH(skel, fifo_ops, scx_fifo);

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 stats[2];
		read_stats(skel, stats);
		printf("local=%llu global=%llu\n", stats[0], stats[1]);
		fflush(stdout);
		sleep(1);
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_fifo__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
