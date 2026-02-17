#include <bpf/bpf.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct proc_stats_val {
	__u64 total_wait_ns;
	__u64 wait_events;
	__u64 cs_in;
	__u64 cpu_ns;
};

struct row {
	__u32 tgid;
	struct proc_stats_val v;
	char comm[64];
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-p PIN_PATH] [-n TOPN]\n\n"
		"  -p PIN_PATH   Pinned map path (default: /sys/fs/bpf/scx_fifo_capture/proc_stats)\n"
		"  -n TOPN       Print top N processes by CPU time (default: all)\n",
		prog);
}

static void read_comm(__u32 tgid, char out[64])
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%u/comm", tgid);
	f = fopen(path, "r");
	if (!f) {
		snprintf(out, 64, "?");
		return;
	}
	if (!fgets(out, 64, f))
		snprintf(out, 64, "?");
	fclose(f);
	out[strcspn(out, "\n")] = '\0';
}

static int cmp_cpu_desc(const void *a, const void *b)
{
	const struct row *ra = a;
	const struct row *rb = b;
	if (ra->v.cpu_ns < rb->v.cpu_ns)
		return 1;
	if (ra->v.cpu_ns > rb->v.cpu_ns)
		return -1;
	return 0;
}

static double ns_to_ms(__u64 ns)
{
	return (double)ns / 1e6;
}

int main(int argc, char **argv)
{
	const char *pin_path = "/sys/fs/bpf/scx_fifo_capture/proc_stats";
	long topn = -1;
	int opt;
	int fd;

	while ((opt = getopt(argc, argv, "p:n:h")) != -1) {
		switch (opt) {
		case 'p':
			pin_path = optarg;
			break;
		case 'n':
			topn = strtol(optarg, NULL, 10);
			break;
		default:
			usage(argv[0]);
			return opt != 'h';
		}
	}

	fd = bpf_obj_get(pin_path);
	if (fd < 0) {
		fprintf(stderr,
			"Failed to open pinned map at %s: %s\n"
			"Make sure the scheduler is running and has pinned the map.\n",
			pin_path, strerror(errno));
		return 1;
	}

	struct row *rows = NULL;
	size_t cap = 0, cnt = 0;
	__u32 key, next_key;
	int ret;

	ret = bpf_map_get_next_key(fd, NULL, &next_key);
	while (ret == 0) {
		struct proc_stats_val v;

		key = next_key;
		if (bpf_map_lookup_elem(fd, &key, &v) == 0) {
			if (cnt == cap) {
				cap = cap ? cap * 2 : 128;
				rows = realloc(rows, cap * sizeof(*rows));
				if (!rows) {
					fprintf(stderr, "Out of memory\n");
					close(fd);
					return 1;
				}
			}
			rows[cnt].tgid = key;
			rows[cnt].v = v;
			read_comm(key, rows[cnt].comm);
			cnt++;
		}

		ret = bpf_map_get_next_key(fd, &key, &next_key);
	}

	if (cnt == 0) {
		printf("No FIFO stats yet.\n");
		free(rows);
		close(fd);
		return 0;
	}

	qsort(rows, cnt, sizeof(*rows), cmp_cpu_desc);

	__u64 sum_wait_ns = 0, sum_wait_ev = 0, sum_cpu_ns = 0, sum_cs = 0;
	for (size_t i = 0; i < cnt; i++) {
		sum_wait_ns += rows[i].v.total_wait_ns;
		sum_wait_ev += rows[i].v.wait_events;
		sum_cpu_ns += rows[i].v.cpu_ns;
		sum_cs += rows[i].v.cs_in;
	}

	double overall_avg_wait_ms = 0.0;
	if (sum_wait_ev)
		overall_avg_wait_ms = ns_to_ms(sum_wait_ns / sum_wait_ev);

	printf("FIFO statistics (per process)\n");
	printf("Pinned map: %s\n\n", pin_path);
	printf("Overall average waiting time: %.3f ms (events=%" PRIu64 ")\n",
		overall_avg_wait_ms, (uint64_t)sum_wait_ev);
	printf("Total CPU time: %.3f ms | Total context switches (in): %" PRIu64 "\n\n",
		ns_to_ms(sum_cpu_ns), (uint64_t)sum_cs);

	printf("%-8s %12s %8s %12s %14s %12s\n",
		"TGID", "CPU(ms)", "CPU%", "CS(in)", "AvgWait(ms)", "WaitEv");
	printf("-----------------------------------------------------------------\n");

	long limit = (topn > 0 && (size_t)topn < cnt) ? topn : (long)cnt;
	for (long i = 0; i < limit; i++) {
		const struct row *r = &rows[i];
		double cpu_ms = ns_to_ms(r->v.cpu_ns);
		double cpu_pct = (sum_cpu_ns ? (100.0 * (double)r->v.cpu_ns / (double)sum_cpu_ns) : 0.0);
		double avg_wait_ms = 0.0;
		if (r->v.wait_events)
			avg_wait_ms = ns_to_ms(r->v.total_wait_ns / r->v.wait_events);

		printf("%-8u %12.3f %7.2f%% %12" PRIu64 " %14.3f %12" PRIu64 "\n",
			r->tgid, cpu_ms, cpu_pct,
			(uint64_t)r->v.cs_in,
			avg_wait_ms,
			(uint64_t)r->v.wait_events);
	}

	free(rows);
	close(fd);
	return 0;
}
