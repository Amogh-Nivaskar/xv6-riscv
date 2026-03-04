#include "kernel/types.h"
#include "user/user.h"
#include "kernel/procinfo.h"
#include "kernel/param.h"

enum loadTypes {CPU, IO};

struct result {
    int pid;
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
            pe->first_runnable_tick = currentry.first_runnable_tick;
            pe->last_runnable_tick = currentry.last_runnable_tick;
            pe->first_run_tick = currentry.first_run_tick;
            pe->runs_count = currentry.runs_count;
            pe->total_wait_time = currentry.total_wait_time;
            pe->exit_tick = currentry.exit_tick;
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

    if (argc > 1 && strcmp(argv[1], "rr") == 0){
        setscheduler(RR);
        printf("Benchmarking Round-Robin Scheduler...\n");
    } else if(argc > 1 && strcmp(argv[1], "mlfq") == 0) {
        setscheduler(MLFQ);
        printf("Benchmarking MLFQ Scheduler...\n");
    }else{
        printf("Need to pass args: rr or mlfq\n");
        exit(1);
    }


    for (int i=0; i < n_cpu; i++){
        pipe(pipes[i]);
        if (fork() == 0){
            // CPU Workload

            close(pipes[i][0]);

            // int N = 500000000;
            volatile int x = 0;

            // for (int round = 0; round < N; round++){
            //     x++;
            // }

            int start = uptime();
            while (uptime() - start < 100){
                x++;
            }
            

            struct result r = {
                .pid = getpid(),
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
            // I/O Workload

            close(pipes[n_cpu + i][0]);

            int N = 200;

            for (int round = 0; round < N; round++){
                sleep(1);
            }

           
            struct result r = {
                .pid = getpid(),
                .load_type = IO
            };
            write(pipes[n_cpu + i][1], &r, sizeof(r));
            exit(0);
        }
        close(pipes[n_cpu + i][1]);
    }

    struct result results[n_cpu + n_io];
    struct procentry entries[n_cpu + n_io];

    for (int i=0; i < n_cpu + n_io; i++){
        read(pipes[i][0], &results[i], sizeof(results[i]));
    }

    sleep(2);

    int ret = getprocinfo(&pi);

    if (ret < 0)
        printf("getprocinfo() FAILED!!");

    for (int i=0; i < n_cpu + n_io; i++){
        int entry_ret = getprocentry(results[i].pid, &pi, &entries[i]);

        if (entry_ret < 0)
            printf("getprocentry() FAILED !!");
    }


    for (int i=0; i < n_cpu + n_io; i++){
        wait(0);
    }

    for (int i=0; i < n_cpu + n_io; i++){
        char *load;
        
        if (results[i].load_type == CPU)
            load = "CPU";
        else
            load = "I/O";
        
        int turnaround_ticks = entries[i].exit_tick - entries[i].first_runnable_tick;
        int response_ticks = entries[i].first_run_tick - entries[i].first_runnable_tick;
        // int avg_wait = entries[i].runs_count > 0 ? entries[i].total_wait_time / entries[i].runs_count : -1;
        // printf("%d, %d\n", entries[i].exit_tick, entries[i].first_run_tick);
        printf("%s PID: %d | Turnaround: %d | Response: %d | Wait: %d\n", load, results[i].pid, turnaround_ticks, response_ticks, entries[i].total_wait_time);
    }
    exit(0);
};

