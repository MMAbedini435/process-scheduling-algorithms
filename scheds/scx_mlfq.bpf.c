/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_mlfq: A minimal 2-level MLFQ scheduler for sched_ext.
 *
 * Policy (as requested):
 *   - All tasks start in the top queue: Round-Robin with a 50ms time slice.
 *   - After a task has executed once in the top RR queue (i.e. it started
 *     running at least once), it is demoted to the bottom queue.
 *   - Bottom queue is FIFO.
 *
 * Implementation notes:
 *   - Uses two shared DSQs: RR_DSQ (top) and FIFO_DSQ (bottom).
 *   - Per-task storage tracks whether the task has already run once at the
 *     top level and what its current level is.
 *   - Dispatch always prefers RR_DSQ over FIFO_DSQ.
 *   - Uses SCX_OPS_SWITCH_PARTIAL by default.
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

enum {
	RR_DSQ		= 0,
	FIFO_DSQ	= 1,
};

enum {
	LVL_RR		= 0,
	LVL_FIFO	= 1,
};

/*
 * Top queue time slice (ns). Default: 50ms.
 * Marked volatile so userspace can override via skeleton rodata.
 */
const volatile u64 rr_slice_ns = 50ULL * 1000ULL * 1000ULL;

/* Bottom queue slice (ns). Default: SCX_SLICE_DFL. */
const volatile u64 fifo_slice_ns = 200ULL * 1000ULL * 1000ULL;

struct task_ctx {
	u8	level;
	u8	ran_top; /* set once when the task first starts running in LVL_RR */
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

/* stats: [0]=local dispatches, [1]=enqueued to RR, [2]=enqueued to FIFO */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, 3);
} stats SEC(".maps");

static __always_inline void stat_inc(u32 idx)
{
	u64 *cnt_p = bpf_map_lookup_elem(&stats, &idx);
	if (cnt_p)
		(*cnt_p)++;
}

static __always_inline u64 slice_for_level(u8 lvl)
{
	return (lvl == LVL_RR) ? rr_slice_ns : fifo_slice_ns;
}

static __always_inline u64 dsq_for_level(u8 lvl)
{
	return (lvl == LVL_RR) ? RR_DSQ : FIFO_DSQ;
}

static __always_inline struct task_ctx *get_tctx(struct task_struct *p)
{
	return bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
}

s32 BPF_STRUCT_OPS(mlfq_select_cpu, struct task_struct *p, s32 prev_cpu,
			  u64 wake_flags)
{
	struct task_ctx *tctx;
	bool is_idle = false;
	s32 cpu;

	cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
	if (!is_idle)
		return cpu;

	/* If we're dispatching directly to local, use the task's current level. */
	tctx = get_tctx(p);
	if (tctx) {
		u64 slice = slice_for_level(tctx->level);

		stat_inc(0);
		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, slice, 0);
	}

	return cpu;
}

void BPF_STRUCT_OPS(mlfq_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx = get_tctx(p);
	u8 lvl = LVL_RR;
	u64 dsq, slice;

	/* If we don't have ctx for some reason, keep the task in the top queue. */
	if (tctx)
		lvl = tctx->level;

	dsq = dsq_for_level(lvl);
	slice = slice_for_level(lvl);

	if (lvl == LVL_RR)
		stat_inc(1);
	else
		stat_inc(2);

	/* FIFO order within each DSQ. RR behavior comes from the time slice. */
	scx_bpf_dispatch(p, dsq, slice, enq_flags);
}

void BPF_STRUCT_OPS(mlfq_dispatch, s32 cpu, struct task_struct *prev)
{
	/* Always prefer top-level RR tasks. */
	if (scx_bpf_consume(RR_DSQ))
		return;
	scx_bpf_consume(FIFO_DSQ);
}

void BPF_STRUCT_OPS(mlfq_running, struct task_struct *p)
{
	struct task_ctx *tctx = get_tctx(p);

	/* Mark first execution in the top queue. */
	if (tctx && tctx->level == LVL_RR && !tctx->ran_top)
		tctx->ran_top = 1;
}

void BPF_STRUCT_OPS(mlfq_stopping, struct task_struct *p, bool runnable)
{
	struct task_ctx *tctx = get_tctx(p);

	/*
	 * After the task has executed once in the RR queue, demote permanently to
	 * the FIFO queue (even if it blocks). This matches the requested behavior.
	 */
	if (tctx && tctx->level == LVL_RR && tctx->ran_top)
		tctx->level = LVL_FIFO;
}

void BPF_STRUCT_OPS(mlfq_enable, struct task_struct *p)
{
	struct task_ctx *tctx = get_tctx(p);

	/* All tasks enter the top RR queue on enable. */
	if (tctx) {
		tctx->level = LVL_RR;
		tctx->ran_top = 0;
	}
}

s32 BPF_STRUCT_OPS(mlfq_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	/* Ensure per-task ctx exists (init_task can allocate). */
	if (bpf_task_storage_get(&task_ctx_stor, p, 0,
				 BPF_LOCAL_STORAGE_GET_F_CREATE))
		return 0;
	return -ENOMEM;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(mlfq_init)
{
	s32 ret;

	ret = scx_bpf_create_dsq(RR_DSQ, -1);
	if (ret)
		return ret;

	return scx_bpf_create_dsq(FIFO_DSQ, -1);
}

void BPF_STRUCT_OPS(mlfq_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(mlfq_ops,
	       .flags			= SCX_OPS_SWITCH_PARTIAL,
	       .select_cpu		= (void *)mlfq_select_cpu,
	       .enqueue		= (void *)mlfq_enqueue,
	       .dispatch		= (void *)mlfq_dispatch,
	       .running		= (void *)mlfq_running,
	       .stopping		= (void *)mlfq_stopping,
	       .enable		= (void *)mlfq_enable,
	       .init_task		= (void *)mlfq_init_task,
	       .init			= (void *)mlfq_init,
	       .exit			= (void *)mlfq_exit,
	       .name			= "mlfq");
