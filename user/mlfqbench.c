#include "kernel/types.h"
#include "user/user.h"
#include "kernel/procinfo.h"


int getprocentry(int pid, struct procinfo* pi, struct procentry* pe){

    for (int i=0; i < pi->count; i++){
        struct procentry currentry = pi->entries[i];
        if (currentry.pid == pid){
            pe->pid = currentry.pid;
            pe->cpu_time = currentry.cpu_time;
            pe->state = currentry.state;
            pe->priority = currentry.priority;
            pe->runnable_tick = currentry.runnable_tick;
            pe->runs_count = currentry.runs_count;
            pe->total_wait_time = currentry.total_wait_time;
            strcpy(pe->name, currentry.name);
            return 0;
        }
    }
    return -1;
}

struct procinfo pi;

int
main(int argc, char *argv[])
{
  
    int n_cpu = 8, n_io = 8;

    for (int i=0; i < n_cpu; i++){
        if (fork() == 0){
            int N = 100000000;
            volatile int x = 0;

            for (int i=0; i < 4; i++){
                for (int round = 0; round < N; round++){
                    x++;
                }
            }

            struct procentry entry;

            int ret = getprocinfo(&pi);

            if (ret < 0)
                printf("getprocinfo() FAILED!!");

            int entry_ret = getprocentry(getpid(), &pi, &entry);

            if (entry_ret < 0)
                printf("getprocentry() FAILED !!");

            int scheduling_freq = entry.runs_count * 100 / uptime();
            printf("CPU PID: %d | Sched Freq: %d\n", entry.pid, scheduling_freq);
            exit(0);

        }
    }
    
    for (int i=0; i < n_io; i++){
        if (fork() == 0){
            int N = 100000;

            for (int i=0; i < 4; i++){

                for (int round = 0; round < N; round++){
                    volatile int x = 0;
                    for(int i = 0; i < 100; i++) x++;  // tiny burst
                    getpid();
                }
            }

            struct procentry entry;

            int ret = getprocinfo(&pi);

            if (ret < 0)
                printf("getprocinfo() FAILED!!");

            int entry_ret = getprocentry(getpid(), &pi, &entry);

            if (entry_ret < 0)
                printf("getprocentry() FAILED !!");

            int scheduling_freq = entry.runs_count * 100 / uptime();
            printf("I/O PID: %d | Sched Freq: %d\n", entry.pid, scheduling_freq);
            exit(0);

        }
    }
    
    
    for (int i=0; i < n_cpu + n_io; i++){
        wait(0);
    }

    exit(0);
};

