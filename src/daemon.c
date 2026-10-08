#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include "probe.skel.h"
#include "pulso.h"

struct task_entry {
	__u32 pid;
	struct task_info info;
};

static volatile sig_atomic_t running = 1;

static struct probe_bpf *probe;
static int cpu_count;

static unsigned long long last_time;
static unsigned long long last_busy[MAX_CPUS];
static unsigned long long last_rx, last_tx, last_read, last_write;

static struct task_entry task_buffers[2][MAX_TASKS];
static struct task_entry *old_tasks = task_buffers[0];
static struct task_entry *new_tasks = task_buffers[1];
static int old_count, new_count;
static struct proc_stat procs[MAX_TASKS];

static void stop(int signal)
{
	(void)signal;
	running = 0;
}

static unsigned long long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int by_pid(const void *a, const void *b)
{
	const struct task_entry *x = a, *y = b;
	return (x->pid > y->pid) - (x->pid < y->pid);
}

static int by_cpu(const void *a, const void *b)
{
	const struct proc_stat *x = a, *y = b;
	return (x->cpu < y->cpu) - (x->cpu > y->cpu);
}

static struct task_entry *find_entry(struct task_entry *entries, int count, __u32 pid)
{
	struct task_entry key = { .pid = pid };
	return bsearch(&key, entries, count, sizeof(key), by_pid);
}

static float percent(unsigned long long part, unsigned long long whole)
{
	return whole ? 100.0f * part / whole : 0;
}

static unsigned long long running_for(int cpu, unsigned long long now)
{
	unsigned long long since = probe->bss->cpu_since[cpu];
	return probe->bss->cpu_task[cpu] && since && now > since ? now - since : 0;
}

static void read_memory(struct snapshot *snapshot)
{
	FILE *file = fopen("/proc/meminfo", "r");
	if (!file)
		return;

	char line[128];
	unsigned long long kb, total = 0, available = 0;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "MemTotal: %llu kB", &kb) == 1)
			total = kb;
		else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1)
			available = kb;
	}
	fclose(file);

	snapshot->mem_total = total * 1024;
	snapshot->mem_used = (total - available) * 1024;
}
