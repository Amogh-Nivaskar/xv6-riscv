#include "kernel/types.h"
#include "user/user.h"
#include "kernel/procinfo.h"
#include "kernel/param.h"

// 8 CPU hogs vs 3 CPUs: each process gets only ~3/8 of a CPU.
// Heavy oversubscription makes scheduling decisions more visible.
#define N_CPU 8
#define N_IO  4
#define N_TOTAL (N_CPU + N_IO)

// Pure computation with NO syscalls in the hot path.
// Previously: uptime() was called every iteration, adding +1 to cpu_time
// per syscall. That caused instant demotion to priority 3 in the first tick
// and masked any difference between schedulers.
// Now: cpu_time only grows via timer interrupt (+10 per tick), so demotion
// happens gradually as the process actually burns CPU time.
// 3B iterations gives each CPU process ~98 CPU-ticks of work,
// enough to be demoted through all 3 MLFQ levels (10+20+40=70 CPU-ticks needed).
// With 8 processes on 3 CPUs: wall-clock ≈ 3B / (11M iter/tick) ≈ 270 ticks.
#define CPU_WORK_ITERS 3000000000L

// I/O burst is tiny (<< 1 tick) so the process is almost never preempted
// during its burst. This keeps its cpu_time low and it stays at high
// MLFQ priority (priority 0-1) throughout the benchmark.
#define IO_BURST_ITERS 10000
#define IO_SLEEP_TICKS 1
// Match CPU duration so I/O and CPU overlap for the full run.
#define IO_ROUNDS      250

enum loadType { CPU, IO };

struct result {
    int pid;
    enum loadType load_type;
};

struct procinfo pi;

int
getprocentry(int pid, struct procentry *pe)
{
    for (int i = 0; i < pi.count; i++) {
        if (pi.entries[i].pid == pid) {
            *pe = pi.entries[i];
            return 0;
        }
    }
    return -1;
}

void
run_bench(int sched_type)
{
    char *name = (sched_type == RR) ? "Round-Robin" : "MLFQ";
    setscheduler(sched_type);
    printf("\n=== %s (%d CPU + %d I/O procs on 3 CPUs) ===\n", name, N_CPU, N_IO);

    int pipes[N_TOTAL][2];

    // Spawn CPU-bound processes
    for (int i = 0; i < N_CPU; i++) {
        pipe(pipes[i]);
        if (fork() == 0) {
            close(pipes[i][0]);
            volatile long x = 0;
            for (long j = 0; j < CPU_WORK_ITERS; j++)
                x++;
            struct result r = { .pid = getpid(), .load_type = CPU };
            write(pipes[i][1], &r, sizeof(r));
            exit(0);
        }
        close(pipes[i][1]);
    }

    // Spawn I/O-bound processes
    for (int i = 0; i < N_IO; i++) {
        pipe(pipes[N_CPU + i]);
        if (fork() == 0) {
            close(pipes[N_CPU + i][0]);
            volatile int y = 0;
            for (int round = 0; round < IO_ROUNDS; round++) {
                // Small burst simulates processing received I/O data.
                // Kept tiny so cpu_time stays low → MLFQ keeps this at priority 0.
                for (int j = 0; j < IO_BURST_ITERS; j++)
                    y++;
                sleep(IO_SLEEP_TICKS);
            }
            struct result r = { .pid = getpid(), .load_type = IO };
            write(pipes[N_CPU + i][1], &r, sizeof(r));
            exit(0);
        }
        close(pipes[N_CPU + i][1]);
    }

    // Collect results: CPU pipes block until each CPU process finishes;
    // I/O pipes are likely already buffered by then.
    // Close read-ends after use so the FD table is clean for the next run.
    struct result results[N_TOTAL];
    for (int i = 0; i < N_TOTAL; i++) {
        read(pipes[i][0], &results[i], sizeof(results[i]));
        close(pipes[i][0]);
    }

    // All children have written and called exit(); give them a moment to
    // become zombies so getprocinfo can capture their final metrics.
    sleep(2);

    if (getprocinfo(&pi) < 0) {
        printf("getprocinfo() FAILED\n");
        exit(1);
    }

    struct procentry entries[N_TOTAL];
    for (int i = 0; i < N_TOTAL; i++) {
        if (getprocentry(results[i].pid, &entries[i]) < 0)
            printf("getprocentry() FAILED for pid %d\n", results[i].pid);
    }

    for (int i = 0; i < N_TOTAL; i++)
        wait(0);

    // xv6 printf only supports bare %s/%d — no width/alignment modifiers.
    // AvgWait (total_wait / runs_count) is omitted: integer division truncates
    // I/O values like 111/250=0.44 to 0, making the column read as either 0 or 1
    // for almost everything. TotalWait carries the same signal without the truncation.
    printf("Type\tPID\tTurnaround\tTotalWait\tPriority\n");
    printf("----\t---\t----------\t---------\t--------\n");

    for (int i = 0; i < N_TOTAL; i++) {
        char *load = (results[i].load_type == CPU) ? "CPU" : "I/O";
        struct procentry *e = &entries[i];
        int turnaround = e->exit_tick - e->first_runnable_tick;
        printf("%s\t%d\t%d\t\t%d\t\t%d\n",
               load, e->pid, turnaround, e->total_wait_time, e->priority);
    }
}

int
main(int argc, char *argv[])
{
    if (argc == 1) {
        // No args: run both schedulers back-to-back for direct comparison.
        run_bench(RR);
        run_bench(MLFQ);
        printf("\nKey: AvgWait = avg ticks per scheduling event\n");
        printf("     MLFQ goal: I/O AvgWait << CPU AvgWait (priority separation)\n");
        printf("     RR  goal:  I/O AvgWait ~= CPU AvgWait (no priority)\n");
    } else if (strcmp(argv[1], "rr") == 0) {
        run_bench(RR);
    } else if (strcmp(argv[1], "mlfq") == 0) {
        run_bench(MLFQ);
    } else {
        printf("Usage: schedbench [rr|mlfq]  (no args runs both)\n");
        exit(1);
    }

    exit(0);
}
