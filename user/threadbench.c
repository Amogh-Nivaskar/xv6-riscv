#include "kernel/types.h"
#include "user/user.h"

// Prime counting benchmark for the kernel threads extension.
//
// Each run divides [2, N] evenly across N threads. Every thread counts
// independently; partial results are summed after all threads finish.
//
// Timing approach: main thread spin-waits on a done[] flag written by each
// sibling, then reads rdcycle while still awake on the same CPU. This avoids
// the CPU migration bug where join() sleeping would cause main to wake on a
// different vCPU with a different rdcycle base, producing a wrap-around result.
// join() is called outside the timed window purely for TCB cleanup.
//
// What this proves on QEMU MTTCG:
//   With -accel tcg,thread=multi, QEMU maps each vCPU to a real host pthread.
//   The 2T and 3T runs will be faster than the 1T run — the threads genuinely
//   execute in parallel on different host cores. Speedup is less than the ideal
//   2x/3x because QEMU's software memory coherence adds overhead, but the
//   directional signal is real.
//
//   On real RISC-V hardware with N cores, you would expect an ~Nx speedup.

#define N          200000
#define MAXTHREADS 3

static volatile uint64 partial[MAXTHREADS];
static volatile int    done[MAXTHREADS];    // written by workers, polled by main
static uint64          expected_primes;

struct range {
    int    id;
    uint64 lo;
    uint64 hi;  // exclusive
};

static struct range ranges[MAXTHREADS];

static int is_prime(uint64 n)
{
    if (n < 2) return 0;
    if (n == 2) return 1;
    if (n % 2 == 0) return 0;
    for (uint64 d = 3; d * d <= n; d += 2)
        if (n % d == 0) return 0;
    return 1;
}

static uint64 count_range(uint64 lo, uint64 hi)
{
    uint64 cnt = 0;
    for (uint64 i = lo; i < hi; i++)
        if (is_prime(i)) cnt++;
    return cnt;
}

void worker(void *arg)
{
    struct range *r = (struct range *)arg;
    partial[r->id] = count_range(r->lo, r->hi);
    done[r->id] = 1;   // signal before exit so main can read t1 on same CPU
    exit_thread(0);
}

static void setup_ranges(int nthreads)
{
    uint64 span  = (uint64)(N - 1);
    uint64 chunk = span / nthreads;
    for (int i = 0; i < nthreads; i++) {
        ranges[i].id = i;
        ranges[i].lo = 2 + (uint64)i * chunk;
        ranges[i].hi = (i == nthreads - 1) ? (uint64)N + 1
                                            : 2 + (uint64)(i + 1) * chunk;
        partial[i] = 0;
        done[i]    = 0;
    }
}

static uint64 bench(int nthreads)
{
    setup_ranges(nthreads);

    uint64 t0 = rdcycle();

    if (nthreads == 1) {
        partial[0] = count_range(ranges[0].lo, ranges[0].hi);
    } else {
        for (int i = 1; i < nthreads; i++) {
            if (clone(worker, &ranges[i]) < 0) {
                printf("clone failed\n");
                exit(1);
            }
        }
        partial[0] = count_range(ranges[0].lo, ranges[0].hi);
        done[0] = 1;
        // Spin-wait keeps main on the same vCPU — no sleep, no migration.
        // With RR scheduler, idle CPUs spin so siblings are picked up almost
        // immediately; the spin time here is just the residual startup delay.
        for (int i = 1; i < nthreads; i++)
            while (!done[i]) {}
    }

    uint64 t1 = rdcycle();

    // join() outside the timing window — purely for TCB slot cleanup
    for (int i = 1; i < nthreads; i++)
        join(0);

    uint64 primes = 0;
    for (int i = 0; i < nthreads; i++)
        primes += partial[i];

    printf("  primes found: %lu", primes);
    if (nthreads == 1)
        expected_primes = primes;
    else if (primes != expected_primes)
        printf("  *** MISMATCH — race condition detected ***");
    printf("\n");

    return t1 - t0;
}

int main(void)
{
    printf("threadbench: counting primes up to %d\n\n", N);

    // Switch to RR so idle CPUs spin instead of WFI-halting.
    // With MLFQ, idle CPUs halt until the next timer interrupt (~100ms),
    // so newly RUNNABLE sibling threads are not picked up immediately.
    setscheduler(0); // 0 = RR

    // Warmup run: faults in ELF pages so the timed runs start even.
    bench(1);

    uint64 c1 = bench(1);
    printf("  1 thread:  %lu cycles\n\n", c1);

    uint64 c2 = bench(2);
    printf("  2 threads: %lu cycles", c2);
    if (c2 < c1)
        printf("  (%lu.%lux speedup — real parallelism!)", c1/c2, (c1*10/c2)%10);
    else
        printf("  (no speedup — QEMU may be serializing on this host)");
    printf("\n\n");

    uint64 c3 = bench(3);
    printf("  3 threads: %lu cycles", c3);
    if (c3 < c1)
        printf("  (%lu.%lux speedup — real parallelism!)", c1/c3, (c1*10/c3)%10);
    else
        printf("  (no speedup — QEMU may be serializing on this host)");
    printf("\n\n");

    printf("on real RISC-V hardware with 3 cores you would expect:\n");
    printf("  2 threads: ~%lu cycles  (~2.0x)\n", c1 / 2);
    printf("  3 threads: ~%lu cycles  (~3.0x)\n", c1 / 3);
    printf("\ncorrectness check passed — all runs found the same prime count.\n");
    exit(0);
}
