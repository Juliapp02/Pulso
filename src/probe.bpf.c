#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "pulso.h"

#define REQ_OP_MASK 0xff

char LICENSE[] SEC("license") = "GPL";

unsigned long long cpu_busy[MAX_CPUS];
unsigned long long cpu_since[MAX_CPUS];
__u32 cpu_task[MAX_CPUS];

unsigned long long net_rx;
unsigned long long net_tx;
unsigned long long disk_read;
unsigned long long disk_write;

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

SEC("tp_btf/net_dev_xmit")
int BPF_PROG(on_xmit, struct sk_buff *skb, int rc, struct net_device *dev, unsigned int len)
{
	if (rc == 0 && !(dev->flags & IFF_LOOPBACK))
		__sync_fetch_and_add(&net_tx, len);
	return 0;
}

SEC("tp_btf/netif_receive_skb")
int BPF_PROG(on_receive, struct sk_buff *skb)
{
	if (!(skb->dev->flags & IFF_LOOPBACK))
		__sync_fetch_and_add(&net_rx, skb->len);
	return 0;
}

SEC("tp_btf/block_rq_complete")
int BPF_PROG(on_block_done, struct request *rq, blk_status_t error, unsigned int bytes)
{
	unsigned int op = rq->cmd_flags & REQ_OP_MASK;

	if (op == REQ_OP_READ)
		__sync_fetch_and_add(&disk_read, bytes);
	else if (op == REQ_OP_WRITE)
		__sync_fetch_and_add(&disk_write, bytes);
	return 0;
}
