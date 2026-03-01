#include "param.h"

struct procentry{
    int pid;
    char name[16];
    int priority;
    int cpu_time;
    int state;
    int total_wait_time;
    int runs_count;
    int runnable_tick;
};

struct procinfo {
    int count;
    struct procentry entries[NPROC];
};