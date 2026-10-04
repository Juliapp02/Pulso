#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "pulso.h"

char LICENSE[] SEC("license") = "GPL";

unsigned long long cpu_busy[MAX_CPUS];
unsigned long long cpu_since[MAX_CPUS];
__u32 cpu_task[MAX_CPUS];

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, MAX_TASKS);
	__type(key, __u32);
	__type(value, struct task_info);
} tasks SEC(".maps");

static struct task_info *find_task(__u32 tgid)
{
	struct task_info *info = bpf_map_lookup_elem(&tasks, &tgid);
	if (info)
		return info;

	struct task_info fresh = {};
	bpf_map_update_elem(&tasks, &tgid, &fresh, BPF_NOEXIST);
	return bpf_map_lookup_elem(&tasks, &tgid);
}

SEC("tp_btf/sched_switch")
int BPF_PROG(on_switch, bool preempt, struct task_struct *prev, struct task_struct *next)
{
	__u32 cpu = bpf_get_smp_processor_id();
	if (cpu >= MAX_CPUS)
		return 0;

	unsigned long long now = bpf_ktime_get_ns();
	unsigned long long since = cpu_since[cpu];
	cpu_since[cpu] = now;
	cpu_task[cpu] = next->pid ? next->tgid : 0;

	if (!since || !prev->pid)
		return 0;

	unsigned long long elapsed = now - since;
	cpu_busy[cpu] += elapsed;

	struct task_info *info = find_task(prev->tgid);
	if (!info)
		return 0;

	__sync_fetch_and_add(&info->runtime, elapsed);
	bpf_probe_read_kernel_str(info->comm, sizeof(info->comm), prev->group_leader->comm);
	return 0;
}
