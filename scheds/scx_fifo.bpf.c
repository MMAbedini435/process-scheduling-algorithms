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
		stat_inc(0);
		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
	}

	return cpu;
}

void BPF_STRUCT_OPS(fifo_enqueue, struct task_struct *p, u64 enq_flags)
{
	stat_inc(1);
	/* Enqueue to the tail of the shared DSQ -> FIFO order. */
	scx_bpf_dispatch(p, FIFO_DSQ, SCX_SLICE_DFL, enq_flags);
}

void BPF_STRUCT_OPS(fifo_dispatch, s32 cpu, struct task_struct *prev)
{
	/* Consume from shared FIFO DSQ whenever this CPU needs a task. */
	scx_bpf_consume(FIFO_DSQ);
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
	       .select_cpu		= (void *)fifo_select_cpu,
	       .enqueue		= (void *)fifo_enqueue,
	       .dispatch		= (void *)fifo_dispatch,
	       .init			= (void *)fifo_init,
	       .exit			= (void *)fifo_exit,
	       .name			= "fifo");
