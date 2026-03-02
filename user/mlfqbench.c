#include "kernel/types.h"
#include "user/user.h"
#include "kernel/procinfo.h"

enum loadTypes {CPU, IO};

struct result {
    int pid;
    int sched_freq;
    enum loadTypes load_type;
};


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
  
    int n_cpu = 4, n_io = 4;
    int pipes[n_cpu + n_io][2];

    printf("Benchmarking MLFQ Schedular...\n");

    for (int i=0; i < n_cpu; i++){
        pipe(pipes[i]);
        if (fork() == 0){
            close(pipes[i][0]);

            int N = 10000000;
            volatile int x = 0;

            for (int round = 0; round < N; round++){
                x++;
            }

            struct procentry entry;

            int ret = getprocinfo(&pi);

            if (ret < 0)
                printf("getprocinfo() FAILED!!");

            int entry_ret = getprocentry(getpid(), &pi, &entry);

            if (entry_ret < 0)
                printf("getprocentry() FAILED !!");

            int scheduling_freq = entry.runs_count * 100 / uptime();
            struct result r = {
                .pid = entry.pid,
                .sched_freq = scheduling_freq,
                .load_type = CPU
            };
            write(pipes[i][1], &r, sizeof(r));
            exit(0);
        }
        close(pipes[i][1]);
    }
    
    for (int i=0; i < n_io; i++){
        pipe(pipes[n_cpu + i]);
        if (fork() == 0){
            close(pipes[n_cpu + i][0]);

            int N = 50;

            for (int round = 0; round < N; round++){
                sleep(1);
            }

            struct procentry entry;

            int ret = getprocinfo(&pi);

            if (ret < 0)
                printf("getprocinfo() FAILED!!");

            int entry_ret = getprocentry(getpid(), &pi, &entry);

            if (entry_ret < 0)
                printf("getprocentry() FAILED !!");

            int scheduling_freq = entry.runs_count * 100 / uptime();
            struct result r = {
                .pid = entry.pid,
                .sched_freq = scheduling_freq,
                .load_type = IO
            };
            write(pipes[n_cpu + i][1], &r, sizeof(r));
            exit(0);
        }
        close(pipes[n_cpu + i][1]);
    }

    struct result r;

    for (int i=0; i < n_cpu + n_io; i++){
        read(pipes[i][0], &r, sizeof(r));
        wait(0);
        char *load;
        
        if (r.load_type == CPU)
            load = "CPU";
        else
            load = "I/O";
        printf("%s PID: %d | Sched Freq: %d\n", load, r.pid, r.sched_freq);
    }
    exit(0);
};

