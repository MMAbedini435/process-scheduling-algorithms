/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_fifo: A minimal global FIFO scheduler for sched_ext.
 *
 * Policy:
 *   - Tasks are enqueued in arrival order (FIFO) into a shared dispatch queue.
 *   - Any CPU which needs work consumes from the shared queue.
 *
 * Notes:
 *   - This is intentionally simple and mirrors the structure of the sample
 *     schedulers (scx_simple / scx_central).
 *   - No priority / vruntime / time accounting beyond the default slice.
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

/*
 * We could directly use SCX_DSQ_GLOBAL for FIFO, but we create our own shared
 * DSQ to match the sample scheduler structure.
 */
#define FIFO_DSQ 0

/* Per-task runtime state for instrumentation */
struct task_ctx {
	u64 enq_ts;	/* when task was enqueued (ready) */
	u64 run_ts;	/* when task started running */
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

/* Per-process (tgid) FIFO statistics */
struct proc_stats_val {
	u64 total_wait_ns;	/* sum of (run_start - enq) */
	u64 wait_events;	/* number of wait samples */
	u64 cs;			/* context switches into the task */
	u64 cpu_ns;		/* CPU time while running */
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 16384);
	__type(key, u32); /* tgid */
	__type(value, struct proc_stats_val);
} proc_stats SEC(".maps");

/* Optional stats: [0]=local dispatches, [1]=global FIFO queue dispatches */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, 2);
} stats SEC(".maps");

static __always_inline void stat_inc(u32 idx)
{
	u64 *cnt_p = bpf_map_lookup_elem(&stats, &idx);
	if (cnt_p)
		(*cnt_p)++;
}

static __always_inline struct task_ctx *get_tctx(struct task_struct *p)
{
	return bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
}

static __always_inline u32 get_tgid(struct task_struct *p)
{
	return (u32)BPF_CORE_READ(p, tgid);
}

static __always_inline struct proc_stats_val *get_pstats(u32 tgid)
{
	struct proc_stats_val *ps;
	struct proc_stats_val zero = {};

	ps = bpf_map_lookup_elem(&proc_stats, &tgid);
	if (ps)
		return ps;
	bpf_map_update_elem(&proc_stats, &tgid, &zero, BPF_NOEXIST);
	return bpf_map_lookup_elem(&proc_stats, &tgid);
}

s32 BPF_STRUCT_OPS(fifo_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	bool is_idle = false;
	s32 cpu;

	cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);

	/*
	 * Fast path: if the chosen CPU is idle, dispatch directly to its local DSQ
	 * to start running immediately.
	 */
	if (is_idle) {
		struct task_ctx *tctx = get_tctx(p);
		if (tctx && !tctx->enq_ts)
			tctx->enq_ts = bpf_ktime_get_ns();
		stat_inc(0);
		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
	}

	return cpu;
}

void BPF_STRUCT_OPS(fifo_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx = get_tctx(p);
	if (tctx)
		tctx->enq_ts = bpf_ktime_get_ns();
	stat_inc(1);
	/* Enqueue to the tail of the shared DSQ -> FIFO order. */
	scx_bpf_dispatch(p, FIFO_DSQ, SCX_SLICE_DFL, enq_flags);
}

void BPF_STRUCT_OPS(fifo_dispatch, s32 cpu, struct task_struct *prev)
{
	/* Consume from shared FIFO DSQ whenever this CPU needs a task. */
	scx_bpf_consume(FIFO_DSQ);
}

void BPF_STRUCT_OPS(fifo_running, struct task_struct *p)
{
	struct task_ctx *tctx = get_tctx(p);
	struct proc_stats_val *ps;
	u64 now;
	u32 tgid;

	if (!tctx)
		return;

	now = bpf_ktime_get_ns();
	tgid = get_tgid(p);
	ps = get_pstats(tgid);
	if (ps) {
		ps->cs++;
		if (tctx->enq_ts) {
			ps->total_wait_ns += now - tctx->enq_ts;
			ps->wait_events++;
			tctx->enq_ts = 0;
		}
	}
	tctx->run_ts = now;
}

void BPF_STRUCT_OPS(fifo_stopping, struct task_struct *p, bool runnable)
{
	struct task_ctx *tctx = get_tctx(p);
	struct proc_stats_val *ps;
	u64 now;
	u32 tgid;

	if (!tctx || !tctx->run_ts)
		return;

	now = bpf_ktime_get_ns();
	tgid = get_tgid(p);
	ps = get_pstats(tgid);
	if (ps)
		ps->cpu_ns += now - tctx->run_ts;
	tctx->run_ts = 0;
}

void BPF_STRUCT_OPS(fifo_enable, struct task_struct *p)
{
	struct task_ctx *tctx = get_tctx(p);
	if (tctx) {
		tctx->enq_ts = 0;
		tctx->run_ts = 0;
	}
}

s32 BPF_STRUCT_OPS(fifo_init_task, struct task_struct *p,
			   struct scx_init_task_args *args)
{
	if (bpf_task_storage_get(&task_ctx_stor, p, 0,
				 BPF_LOCAL_STORAGE_GET_F_CREATE))
		return 0;
	return -ENOMEM;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(fifo_init)
{
	return scx_bpf_create_dsq(FIFO_DSQ, -1);
}

void BPF_STRUCT_OPS(fifo_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(fifo_ops,
	       .flags			= SCX_OPS_SWITCH_PARTIAL,
	       .select_cpu		= (void *)fifo_select_cpu,
	       .enqueue		= (void *)fifo_enqueue,
	       .dispatch		= (void *)fifo_dispatch,
	       .running		= (void *)fifo_running,
	       .stopping		= (void *)fifo_stopping,
	       .enable		= (void *)fifo_enable,
	       .init_task		= (void *)fifo_init_task,
	       .init			= (void *)fifo_init,
	       .exit			= (void *)fifo_exit,
	       .name			= "fifo");
