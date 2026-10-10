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

static void collect_tasks(unsigned long long now)
{
	int map = bpf_map__fd(probe->maps.tasks);
	__u32 key, next;
	__u32 *cursor = NULL;
	__u32 dead[MAX_TASKS];
	int dead_count = 0;

	new_count = 0;
	while (new_count < MAX_TASKS && bpf_map_get_next_key(map, cursor, &next) == 0) {
		key = next;
		cursor = &key;

		if (kill(key, 0) && errno == ESRCH) {
			dead[dead_count++] = key;
			continue;
		}

		struct task_entry *entry = &new_tasks[new_count];
		entry->pid = key;
		if (bpf_map_lookup_elem(map, &key, &entry->info) == 0)
			new_count++;
	}

	for (int i = 0; i < dead_count; i++)
		bpf_map_delete_elem(map, &dead[i]);

	qsort(new_tasks, new_count, sizeof(*new_tasks), by_pid);

	for (int cpu = 0; cpu < cpu_count; cpu++) {
		struct task_entry *entry = find_entry(new_tasks, new_count, probe->bss->cpu_task[cpu]);
		if (entry)
			entry->info.runtime += running_for(cpu, now);
	}
}

static void rank_processes(struct snapshot *snapshot, unsigned long long elapsed)
{
	for (int i = 0; i < new_count; i++) {
		struct task_entry *now = &new_tasks[i];
		struct task_entry *before = find_entry(old_tasks, old_count, now->pid);
		unsigned long long used = now->info.runtime;

		if (before)
			used = used > before->info.runtime ? used - before->info.runtime : 0;

		procs[i].pid = now->pid;
		procs[i].cpu = percent(used, elapsed);
		memcpy(procs[i].comm, now->info.comm, COMM_LEN);
	}

	qsort(procs, new_count, sizeof(*procs), by_cpu);
	snapshot->proc_count = new_count < MAX_PROCS ? new_count : MAX_PROCS;
	memcpy(snapshot->procs, procs, snapshot->proc_count * sizeof(*procs));

	struct task_entry *swap = old_tasks;
	old_tasks = new_tasks;
	new_tasks = swap;
	old_count = new_count;
}

static double rate(unsigned long long current, unsigned long long *last, double seconds)
{
	double value = (current - *last) / seconds;
	*last = current;
	return value;
}

static void take_snapshot(struct snapshot *snapshot)
{
	unsigned long long now = now_ns();
	unsigned long long elapsed = now - last_time;
	double seconds = elapsed / 1e9;
	float total = 0;

	snapshot->tick++;
	snapshot->cpu_count = cpu_count;
	for (int cpu = 0; cpu < cpu_count; cpu++) {
		unsigned long long busy = probe->bss->cpu_busy[cpu] + running_for(cpu, now);
		float usage = busy > last_busy[cpu] ? percent(busy - last_busy[cpu], elapsed) : 0;

		snapshot->cpu[cpu] = usage > 100 ? 100 : usage;
		total += snapshot->cpu[cpu];
		last_busy[cpu] = busy;
	}
	snapshot->cpu_total = total / cpu_count;

	snapshot->net_rx = rate(probe->bss->net_rx, &last_rx, seconds);
	snapshot->net_tx = rate(probe->bss->net_tx, &last_tx, seconds);
	snapshot->disk_read = rate(probe->bss->disk_read, &last_read, seconds);
	snapshot->disk_write = rate(probe->bss->disk_write, &last_write, seconds);

	read_memory(snapshot);
	collect_tasks(now);
	rank_processes(snapshot, elapsed);
	last_time = now;
}

static int open_socket(void)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = SOCKET_PATH };
	int server = socket(AF_UNIX, SOCK_STREAM, 0);

	unlink(SOCKET_PATH);
	if (server < 0 || bind(server, (struct sockaddr *)&address, sizeof(address)) || listen(server, 16)) {
		perror("socket");
		return -1;
	}
	chmod(SOCKET_PATH, 0666);
	return server;
}

static void serve(int server, const struct snapshot *snapshot, int timeout_ms)
{
	struct pollfd waiting = { .fd = server, .events = POLLIN };

	while (running && poll(&waiting, 1, timeout_ms) > 0) {
		int client = accept(server, NULL, NULL);
		if (client >= 0) {
			send(client, snapshot, sizeof(*snapshot), MSG_NOSIGNAL);
			close(client);
		}
		timeout_ms = 0;
	}
}

int main(void)
{
	probe = probe_bpf__open_and_load();
	if (!probe || probe_bpf__attach(probe)) {
		fprintf(stderr, "pulsod: could not load eBPF probe (are you root?)\n");
		return 1;
	}

	int server = open_socket();
	if (server < 0)
		return 1;

	signal(SIGINT, stop);
	signal(SIGTERM, stop);

	cpu_count = sysconf(_SC_NPROCESSORS_CONF);
	if (cpu_count > MAX_CPUS)
		cpu_count = MAX_CPUS;

	static struct snapshot snapshot;
	last_time = now_ns();
	unsigned long long next_tick = last_time + 1000000000ULL;

	printf("pulsod: serving on %s\n", SOCKET_PATH);
	while (running) {
		unsigned long long now = now_ns();
		if (now >= next_tick) {
			take_snapshot(&snapshot);
			next_tick += 1000000000ULL;
			continue;
		}
		serve(server, &snapshot, (next_tick - now) / 1000000 + 1);
	}

	close(server);
	unlink(SOCKET_PATH);
	probe_bpf__destroy(probe);
	return 0;
}
