# Building a Multi-Level Feedback Queue Scheduler in xv6

*The full xv6 implementation, benchmarks, and test programs are available on* [*GitHub*](https://github.com/Amogh-Nivaskar/xv6-riscv/tree/mlfq-scheduler)

## The Problem

xv6's default round-robin scheduler has no concept of what a process is doing with its
time slice. A shell waiting for a keypress gets the same treatment as a loop burning 10
million arithmetic operations — both sit in the same queue, both wait their turn, both
get exactly one timer tick of CPU. On a 3-CPU QEMU instance running a mixed workload,
a freshly typed command waits through the entire rotation of every CPU-bound process
ahead of it before the shell even sees the keystroke. That rotation can span dozens of
active processes. The response latency is proportional to queue depth, not to urgency.
Round-robin is fair in the accounting sense and completely wrong in the human sense.

---

## What Is MLFQ?

Multi-Level Feedback Queue (MLFQ) is a scheduling algorithm that solves the core
problem of round-robin: it learns from a process's past behavior and adjusts its
priority accordingly — without requiring the programmer to classify processes upfront.

The core insight: **processes reveal their nature through their behavior**. An
interactive shell that blocks on I/O frequently never exhausts its CPU allotment, so
it stays at the top of the queue. A CPU-bound batch job that runs until it's preempted
keeps consuming its full allotment and gets pushed down. No manual tagging required.

### Structure

MLFQ maintains multiple queues, each representing a priority level. Processes start at
the top (highest priority) and migrate downward as they consume CPU time.

```
  Priority 0 (highest) — Allotment: 100 units
  ┌──────────────────────────────────────┐
  │  [shell]  [editor]   ← interactive  │
  └──────────────────────────────────────┘
                  │ exhaust allotment
                  ▼
  Priority 1 — Allotment: 200 units
  ┌──────────────────────────────────────┐
  │  [medium CPU job]                    │
  └──────────────────────────────────────┘
                  │ exhaust allotment
                  ▼
  Priority 2 — Allotment: 400 units
  ┌──────────────────────────────────────┐
  │  [compiler]                          │
  └──────────────────────────────────────┘
                  │ exhaust allotment
                  ▼
  Priority 3 (lowest) — Allotment: 800 units
  ┌──────────────────────────────────────┐
  │  [heavy batch jobs]   ← CPU-bound   │
  └──────────────────────────────────────┘
```

### Rules

1. **Priority order**: Always run the `RUNNABLE` process at the highest priority queue.
2. **Demotion**: When a process exhausts its allotment at level *N*, move it to level *N+1* and reset its accumulated time.
3. **I/O shortcut**: A process that blocks on I/O before exhausting its allotment keeps its current priority — it didn't consume what it was given.
4. **Priority boost**: Periodically, reset all processes to priority 0. This prevents starvation of processes that have drifted to the bottom.

### How It Adapts Over Time

```
  New process arrives
         │
         ▼
  ┌──────────────┐
  │  Priority 0  │── uses full allotment
  │  (top queue) │        ──► P1 ──► ... ──► P3
  └──────────────┘
         │
         │ blocks on I/O before allotment
         ▼
  ┌──────────────┐
  │   SLEEPING   │── wakeup() ──► stays at Priority 0
  └──────────────┘               (interactive reward)
```

The feedback loop is entirely implicit — no system calls, no hints from the programmer.
A process that behaves like a CPU hog gets treated like one. A process that behaves like
an interactive task gets low latency, automatically.

---

## The Naive Solution and Why It Fails

The original xv6 scheduler is a textbook round-robin: scan the process table, pick the
first `RUNNABLE` entry, run it, repeat.

```c
void sched_rr(struct cpu *c) {
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->state == RUNNABLE) {
            p->state = RUNNING;
            c->proc = p;
            swtch(&c->context, &p->context);
            c->proc = 0;
        }
        release(&p->lock);
    }
}
```

The assumption embedded here is that equal time means fair treatment. That breaks the
moment the workload is heterogeneous.

A CPU-bound process will use every nanosecond of its quantum doing real computation.
An I/O-bound process — a shell, a network handler, anything waiting for human input —
will use a fraction of its quantum, block on I/O, sleep, and wake up back in the queue.
Under round-robin, both processes look identical to the scheduler. The shell that woke
from a `read()` call, ready to respond to a keystroke, waits in the same queue behind
a matrix multiply that holds the CPU for its full tick.

```
  Round-Robin queue (all processes treated equally):

  ┌─────────┬───────────┬─────────┬───────────┐
  │  shell  │ compiler  │  shell  │ compiler  │ ...
  │ (1 tick)│ (10 ticks)│ (1 tick)│ (10 ticks)│
  └─────────┴───────────┴─────────┴───────────┘
       ▲                                  │
       └──────────────────────────────────┘
  Shell waits through every compiler turn
  before getting scheduled again.
```

The cost is entirely in first-response latency. For a CPU-bound batch job, waiting one
extra rotation through the queue is invisible. For an interactive shell, that same wait
is the gap between keypress and echo — the difference between feeling instant and
feeling sluggish.

---

## Design Decisions

### Allotment sizing and the doubling schedule

The first decision was how long a process gets to stay at each priority level before
being demoted. The obvious choice — equal allotments at every level — means CPU-bound
processes thrash between levels and level transitions have no predictive value. I chose
doubling allotments: `{100, 200, 400, 800}` cpu_time units across the four levels.

The logic: a process that exhausts its allotment at level 0 has demonstrated it's
CPU-hungry. It deserves more time at the next level before being judged again, not the
same brief window. The doubling schedule means a process settles into its natural level
quickly and then stays there, rather than bouncing. The tradeoff accepted is that
demotion to the bottom takes longer, but the bottom queue is where long-running CPU jobs
belong.

### What counts as cpu_time — and why syscalls are charged

I needed a unit of measurement that captures actual CPU consumption. The obvious approach
is to increment `cpu_time` only on timer interrupts — each tick is 10 units, demotion
happens after allotment/10 ticks. This is where a subtle vulnerability appears, which
I'll describe in detail in the [future work](#what-id-do-differently--whats-next) section.

The fix baked into the current implementation: syscalls are also charged. Every trap into
the kernel that represents a syscall increments `cpu_time` by 1. Timer interrupts
increment it by 10. The comment in `kernel/trap.c` is blunt about the reason:

```c
// increase cpu_time to avoid gaming of schedular
p->cpu_time += 1;
```

An I/O-bound process that makes many syscalls accumulates cpu_time gradually, which is
correct — it is consuming CPU for syscall overhead. A CPU-bound process that mostly
loops between timer interrupts accumulates it in large jumps of 10.

```
  cpu_time accumulation over time:

  CPU-bound process:
  ┌─────────────────────────────────────────┐
  │  timer  timer  timer  timer  timer  ... │
  │  +10    +10    +10    +10    +10        │
  └─────────────────────────────────────────┘
  → demoted quickly

  I/O-bound process:
  ┌─────────────────────────────────────────┐
  │ syscall sleep  syscall sleep  syscall   │
  │  +1    (0)     +1    (0)     +1         │
  └─────────────────────────────────────────┘
  → stays at high priority
```

### The priority boost — preventing starvation

Without a priority boost, a process stuck at level 3 stays there forever if higher
queues are never empty. Two scenarios cause this:

1. A burst of CPU-bound work dominates all queues; lower-level processes never get a turn.
2. A process that starts CPU-bound and later becomes interactive is stuck at the level
   it was demoted to, missing the responsiveness it now deserves.

The fix is periodic: every `MLFQ_BOOST` ticks (500 — about 5 seconds), every process
gets reset to priority 0 with cpu_time cleared.

```
  Before boost:
  Priority 0: [shell]
  Priority 1: []
  Priority 2: [batch1]
  Priority 3: [batch2] [compiler]

        │
        │ every 500 ticks: reset all to priority 0
        ▼

  After boost:
  Priority 0: [shell] [batch1] [batch2] [compiler]
  Priority 1: []
  Priority 2: []
  Priority 3: []
```

The implementation does this inline in the scan loop — all processes either get boosted
in one pass or none do. No dedicated timer, no additional synchronization surface.

### Per-CPU chosen_proc — solving the TOCTOU race

The scan and the context switch are not atomic. Between the moment `sched_mlfq`
identifies the best candidate and the moment it calls `swtch()`, another CPU could
have picked up the same process. The solution is a per-CPU `chosen_proc` pointer in
`sched_state`. After the scan, the chosen process is re-checked under its lock:

```c
if (ss->chosen_proc->state == RUNNABLE) {
    ss->chosen_proc->state = RUNNING;
    swtch(&c->context, &ss->chosen_proc->context);
}
```

If the re-check fails, the current CPU skips this round. No double-scheduling, no
corrupted process state. The cost is one wasted scan per conflict, which is rare on a
3-CPU QEMU instance.

### Idle behavior — wfi instead of spin

The original round-robin wastes an entire core when there are no RUNNABLE processes —
the scan completes, finds nothing, and immediately restarts. The MLFQ implementation
falls back to `wfi` (Wait For Interrupt) when the scan finds nothing:

```c
} else {
    asm volatile("wfi");
}
```

On QEMU, a CPU spinning in a tight loop burns host CPU time doing nothing useful. `wfi`
parks the hart until the next interrupt arrives, at which point the scheduler
re-evaluates.

---

## The Implementation

The core of `sched_mlfq` does three things in a single pass over the process table:
boost if needed, demote if overdue, and select the highest-priority RUNNABLE process.

```c
void sched_mlfq(struct cpu *c) {
    struct sched_state *ss = &cpu_sched_state[cpuid()];
    static const int mlfq_allotment[NMLFQ] = {100, 200, 400, 800};
    int boost_now = (ticks - ss->last_boost_tick >= MLFQ_BOOST);

    int found = 0;
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (boost_now && p->state != UNUSED) {
            p->cpu_time = 0;
            p->priority = 0;
        }
        if (p->state != UNUSED && p->cpu_time >= mlfq_allotment[p->priority]) {
            if (p->priority < NMLFQ - 1) {
                p->priority += 1;
                p->cpu_time = 0;
            }
        }
        if (p->state == RUNNABLE &&
            (ss->chosen_proc == NULL || ss->chosen_proc->priority > p->priority)) {
            ss->chosen_proc = p;
            found = 1;
        }
        release(&p->lock);
    }
```

The `chosen_proc == NULL || chosen_proc->priority > p->priority` condition is the MLFQ
selection rule: among all RUNNABLE processes, always prefer the one at the highest queue
(lowest `priority` number). Multiple processes at the same priority level get round-robin
treatment implicitly — whichever appears first in the table wins each scan.

The non-obvious edge: boost and demotion happen in the same pass that selects the
candidate. A process that gets boosted from level 3 to level 0 this tick is immediately
eligible to be the chosen candidate. The boost takes effect on the very next scheduling
decision, not a pass later.

The complete lifecycle of a process through the scheduler looks like this:

```
  fork() / allocproc()
        │
        │  priority = 0, cpu_time = 0
        ▼
  ┌──────────────────────────────────────────────────────┐
  │                   MLFQ Scheduler Loop                │
  │                                                      │
  │  1. Boost? (ticks - last_boost >= 500)               │
  │     → reset ALL procs: priority=0, cpu_time=0        │
  │                                                      │
  │  2. Demote? (cpu_time >= allotment[priority])        │
  │     → priority++, cpu_time=0                         │
  │                                                      │
  │  3. Select? (RUNNABLE && lowest priority number)     │
  │     → chosen_proc = p                                │
  └──────────────────────────────────────────────────────┘
        │
        │  swtch() → process runs
        ▼
  ┌──────────────────────────────────────┐
  │               RUNNING               │
  └──────────────────────────────────────┘
        │
        ├── timer fires → cpu_time += 10
        │   → yield() → back to scheduler
        │
        ├── syscall → cpu_time += 1
        │   → continues running
        │
        └── I/O / sleep → SLEEPING
            cpu_time frozen, priority kept
```

The observability infrastructure is wired directly into the switch path, feeding the
`getprocinfo()` syscall that `schedbench` uses to compute per-process turnaround,
response, and wait times after the workload completes. Debugging scheduler behavior
without this visibility would be guesswork.

---

## Benchmarks

### Test Design

The benchmark (`user/schedbench.c`) runs both schedulers back-to-back in a single
invocation, switching via `setscheduler()` between rounds. Round-Robin runs first,
MLFQ second; the same binary drives both with identical child counts and workloads.

Each round spawns 12 children: 8 CPU-bound and 4 I/O-bound, against 3 CPUs — a
4× oversubscription ratio that creates meaningful scheduling pressure.

**CPU-bound workload:** Each process runs a tight arithmetic loop for 3 billion
iterations with no syscalls in the hot path. Without syscalls in the loop, `cpu_time`
only increments by 10 per timer interrupt, so demotion happens gradually as the process
burns through its CPU allotment at each priority level. The 3 billion iteration count
is sized so each process accumulates roughly 98 CPU-ticks of work — enough to be
demoted through all three thresholds (10 ticks at priority 0, 20 more at priority 1,
40 more at priority 2) and settle at priority 3 before exiting.

**I/O-bound workload:** Each process loops 250 times: a 10,000-iteration CPU burst
followed by `sleep(1)`. The burst is kept intentionally tiny (well under 1 timer tick)
so the process accumulates almost no `cpu_time` from timer interrupts — only the
syscall overhead (+1 per `sleep()` call) accumulates. After 250 rounds, accumulated
`cpu_time` is ~250, enough for one demotion to priority 1 but never deep enough to
compete with demoted CPU processes at priority 2–3.

Results are collected through the `getprocinfo()` syscall after all children have exited
and entered zombie state, capturing final `priority` and `total_wait_time` per process.

Response time (`first_run_tick - first_runnable_tick`) was omitted from the table:
because all 12 processes fork within a few ticks of each other and all start at
priority 0 in both schedulers, initial response times are nearly identical. The
per-cycle wait accumulated in `TotalWait` captures the more meaningful ongoing
scheduling behavior once demotion has taken effect.

### Results

```
=== Round-Robin (8 CPU + 4 I/O procs on 3 CPUs) ===
Type    PID     Turnaround      TotalWait       Priority
----    ---     ----------      ---------       --------
CPU     4       189             115             0
CPU     5       189             115             0
CPU     6       197             122             0
CPU     7       197             122             0
CPU     8       193             120             0
CPU     9       197             123             0
CPU     10      197             124             0
CPU     11      197             124             0
I/O     12      361             111             0
I/O     13      361             111             0
I/O     14      361             111             0
I/O     15      361             111             0

=== MLFQ (8 CPU + 4 I/O procs on 3 CPUs) ===
Type    PID     Turnaround      TotalWait       Priority
----    ---     ----------      ---------       --------
CPU     16      112             40              2
CPU     17      126             51              2
CPU     18      179             106             3
CPU     19      182             109             3
CPU     20      185             112             3
CPU     21      188             118             3
CPU     22      193             131             3
CPU     23      206             147             3
I/O     24      275             25              1
I/O     25      275             25              1
I/O     26      275             25              1
I/O     27      275             25              1
```

### Analysis

**CPU Priority — the clearest signal**

In Round-Robin, every CPU process exits at priority 0. The scheduler has no concept of
what the process did with its time; every process looks identical to it. In MLFQ, every
CPU process is demoted — 6 of 8 reach priority 3 (all three demotion thresholds
crossed), and the other 2 land at priority 2. The scheduler correctly identified them
as CPU hogs and pushed them down without any programmer annotation.

**I/O total wait — 4.4× reduction**

```
I/O TotalWait (accumulated across 250 sleep cycles):
  Round-Robin:  111 ticks
  MLFQ:          25 ticks
```

Each I/O process wakes from `sleep()` 250 times. In Round-Robin, that process wakes
into a queue of 8 CPU-bound competitors at equal priority and must wait its turn before
any CPU becomes available. In MLFQ, the I/O process wakes at priority 0 or 1 — above
all the demoted CPU hogs at priority 2–3 — and is scheduled on the next available CPU
before any of them. Across 250 cycles, the accumulated wait drops from 111 ticks to 25.

**I/O turnaround — 24% faster**

```
I/O Turnaround:
  Round-Robin:  361 ticks
  MLFQ:         275 ticks   (-86 ticks, -24%)
```

Because I/O processes spend less time queued behind CPU hogs, their total wall-clock
completion time is significantly lower. The 86-tick gap is entirely explained by the
TotalWait reduction (111 → 25 = 86 fewer ticks of waiting).

**CPU turnaround — higher variance in MLFQ, not a flaw**

In Round-Robin, all 8 CPU processes finish within an 8-tick band (189–197). Equal
shares mean they race to the finish together. In MLFQ, the band widens to 94 ticks
(112–206). The first two processes finish in 112 and 126 ticks — faster than any
Round-Robin result — because they get more CPU early before the field is fully demoted.
At the start of the MLFQ run, all 8 CPU processes sit at priority 0, but only 3 CPUs
are available. The first 3 initial scan winners get uncontested CPU time — but one slot likely went to an I/O process that slept almost immediately, leaving PIDs 16 and 17 as the two CPU processes that ran with no peer competition until their first demotion. The head start
compounds through each subsequent demotion boundary. The last process finishes at 206
ticks, slightly slower than Round-Robin's worst, because by that point everything is
piled at priority 3 and it is contending with 7 peers for 3 CPUs. The variance is an
accurate reflection of timing luck under a priority-aware scheduler.

### Conclusion

The benchmark confirms the three core behaviors of MLFQ working correctly in xv6:

1. **CPU hogs are detected and penalized.** Without a single hint from the programmer,
   every CPU-bound process was demoted to priority 2 or 3 by the time it exited.
   Round-Robin cannot distinguish them from any other process.

2. **I/O-bound processes are protected.** All four I/O processes exited at priority 1,
   having never accumulated enough `cpu_time` to be pushed lower. They received
   preferential scheduling throughout the run.

3. **Lower priority directly translates to lower latency for interactive work.** The
   4.4× reduction in I/O wait time and 24% reduction in I/O turnaround are direct
   consequences of I/O processes jumping the queue every time they woke from sleep.
   In a real system, this is the difference between a shell command that echoes
   instantly and one that visibly lags while a compile job runs in the background.

---

## What I'd Do Differently / What's Next

The most significant limitation in the current implementation is a scheduler gaming
vulnerability that is subtle enough to deserve a precise description.

A process that calls `yield()` — which is a syscall — at the right moment accumulates
only 1 unit of cpu_time from the syscall entry, not 10 from a timer interrupt. The
timer fires after the process has already yielded the CPU; its `+10` goes to the next
running process. The exploit loop: yield just before the tick fires, wake up as `RUNNABLE`
with minimal cpu_time, get rescheduled immediately, repeat.

```
  Gaming process (yield just before each tick):

  ┌── RUNNING ──┐       ┌── RUNNING ──┐
  │             │ yield │             │ yield
──┘             └───────┘             └────────►
  t=0        t≈9 t=10             t≈19 t=20
                  │                    │
              timer fires          timer fires
              process idle         process idle
              → no +10             → no +10

  cpu_time:  0 ────► 1      1 ────► 2    2 ──► ...

  Each yield() costs +1. Timer +10 never lands.
  Needs 100 yields to reach allotment=100.
  An honest process takes only 10 timer ticks.


  Honest process (runs until preempted by timer):

  ┌──────────────── RUNNING ──────────────────────►
  │              │              │
  t=0        timer fires    timer fires
               t=10            t=20
               +10              +10

  cpu_time: 0 ─────────► 10 ─────────► 20 ──► ... 100 → DEMOTED
```

The current implementation already has a partial mitigation for this: every syscall
entry charges `cpu_time += 1`. A gaming process that calls `yield()` repeatedly still
accumulates 1 unit per call, so it cannot stay at priority 0 forever — it just
accumulates slowly rather than in 10-unit timer jumps. This raises the cost of gaming
considerably compared to a naive implementation that only charges on timer interrupts.
It is not a complete fix — a process with the right timing can still game more slowly
than an honest CPU hog — but it makes the exploit meaningfully harder and is why the
syscall charge was added to `kernel/trap.c` in the first place.

The correct fix is to measure CPU time in **cycles**, not ticks. The RISC-V `rdcycle`
CSR provides a 64-bit monotonically increasing cycle counter. Charge the process for
cycles elapsed between when it was scheduled and when it next yields — there is no tick
boundary to time around. A process that calls `yield()` a nanosecond before a timer
edge is charged the cycles it actually consumed, nothing more, nothing less.

The reason this isn't in the current implementation: at the time the MLFQ was written,
xv6 didn't expose `rdcycle` through `scounteren` in a way the scheduler could easily
consume. Supervisor-mode cycle counter access requires setting the appropriate bits in
`scounteren`, which was added in a later extension to this codebase. The infrastructure
was in reach, but not yet wired up.

One thing I got wrong in the first implementation: `last_runnable_tick` was not being
updated when a sleeping process woke via `wakeup()`. Wait time is computed as
`ticks - last_runnable_tick` at scheduling time, so a process that slept intentionally
for 200 ticks and then waited 5 ticks for the CPU reported 205 ticks of wait time —
inflating the number with sleep time rather than CPU-wait time. The fix was updating
`last_runnable_tick` inside `wakeup()`, not only inside `yield()`.

A production MLFQ would replace the O(NPROC) linear scan with per-priority runqueues:
one linked list per level, selection in O(1) by taking the head of the highest non-empty
queue. At NPROC=64 the scan cost is negligible, but the algorithm's selection time grows
with the process table and should not.
