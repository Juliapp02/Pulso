#ifndef PULSO_H
#define PULSO_H

#define SOCKET_PATH "/run/pulso.sock"
#define MAX_CPUS 256
#define MAX_TASKS 16384
#define MAX_PROCS 64
#define COMM_LEN 16

struct task_info {
	unsigned long long runtime;
	char comm[COMM_LEN];
};

struct proc_stat {
	int pid;
	float cpu;
	char comm[COMM_LEN];
};

struct snapshot {
	unsigned int tick;
	int cpu_count;
	float cpu_total;
	float cpu[MAX_CPUS];
	unsigned long long mem_total;
	unsigned long long mem_used;
	double net_rx;
	double net_tx;
	double disk_read;
	double disk_write;
	int proc_count;
	struct proc_stat procs[MAX_PROCS];
};

#endif
