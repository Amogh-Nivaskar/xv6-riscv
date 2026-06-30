# Building a Multi-Level Feedback Queue Scheduler in xv6

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
  Priority 0 (highest)  ┌───────────────────────────────┐
  Allotment: 100 units   │  [shell]  [editor]            │  ← Interactive processes
                         └───────────────────────────────┘
                                        │ exhaust allotment
                                        ▼
  Priority 1             ┌───────────────────────────────┐
  Allotment: 200 units   │  [medium CPU job]             │
                         └───────────────────────────────┘
                                        │ exhaust allotment
                                        ▼
  Priority 2             ┌───────────────────────────────┐
  Allotment: 400 units   │  [compiler]                   │
                         └───────────────────────────────┘
                                        │ exhaust allotment
                                        ▼
  Priority 3 (lowest)   ┌───────────────────────────────┐
  Allotment: 800 units   │  [heavy batch jobs]           │  ← CPU-bound processes
                         └───────────────────────────────┘
```

### Rules

1. **Priority order**: Always run the RUNNABLE process at the highest priority queue.
2. **Demotion**: When a process exhausts its allotment at level *N*, move it to level *N+1* and reset its accumulated time.
3. **I/O shortcut**: A process that blocks on I/O before exhausting its allotment keeps its current priority — it didn't consume what it was given.
4. **Priority boost**: Periodically, reset all processes to priority 0. This prevents starvation of processes that have drifted to the bottom.

### How It Adapts Over Time

```
  New process arrives
        │
        ▼
  ┌──────────────┐    Uses full allotment?    ┌──────────────┐
  │  Priority 0  │ ─── YES ─────────────────► │  Priority 1  │ ──► ... ──► Priority 3
  │  (top queue) │                            └──────────────┘
  └──────────────┘
        │
        │  Blocks on I/O before allotment?
        ▼
  Stays at Priority 0  ◄──────── wakeup() ──────── SLEEPING
  (interactive reward)
```

The feedback loop is entirely implicit — no system calls, no hints from the programmer.
A process that behaves like a CPU hog gets treated like one. A process that behaves like
an interactive task gets low latency, automatically.

---

## The Naive Solution and Why It Fails

The original xv6 scheduler is a textbook round-robin: scan the process table, pick the
first RUNNABLE entry, run it, repeat.

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

  ┌──────────┬──────────┬──────────┬──────────┬──────────┐
  │  shell   │ compiler │  shell   │ compiler │  shell   │  ...
  │ (1 tick) │(10 ticks)│ (1 tick) │(10 ticks)│ (1 tick) │
  └──────────┴──────────┴──────────┴──────────┴──────────┘
       ▲                                            │
       └────────────────────────────────────────────┘
       Shell waits through every compiler turn before responding
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
I'll describe in detail in the Future Work section.

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
  │  +10    +10    +10    +10    +10        │  → demoted quickly
  └─────────────────────────────────────────┘

  I/O-bound process:
  ┌─────────────────────────────────────────┐
  │ syscall sleep  syscall sleep  syscall   │
  │  +1    (0)     +1    (0)     +1         │  → stays at high priority
  └─────────────────────────────────────────┘
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
  Before boost:                       After boost (every 500 ticks):

  Priority 0: [shell]                 Priority 0: [shell] [batch1] [batch2] [compiler]
  Priority 1: []
  Priority 2: [batch1]          ──►   Priority 1: []
  Priority 3: [batch2][compiler]      Priority 2: []
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
  ┌──────────────┐     timer interrupt      ┌────────────────────┐
  │   RUNNING    │ ──── cpu_time += 10 ───► │  yield() → sched() │
  │              │     → yield()            └────────────────────┘
  │              │
  │              │     syscall              ┌────────────────────┐
  │              │ ──── cpu_time += 1  ───► │  continues running │
  │              │                          └────────────────────┘
  │              │
  │              │     I/O / sleep          ┌────────────────────┐
  │              │ ────────────────────────►│  SLEEPING          │
  └──────────────┘                          │  cpu_time frozen   │
                                            │  priority kept     │
                                            └────────────────────┘
```

The observability infrastructure is wired directly into the switch path, feeding the
`getprocinfo()` syscall that `schedbench` uses to compute per-process turnaround,
response, and wait times after the workload completes. Debugging scheduler behavior
without this visibility would be guesswork.

---

## Benchmarks

*Results comparing MLFQ against Round-Robin across mixed CPU/IO workloads will be filled
in after running `schedbench rr` and `schedbench mlfq` on a live xv6 instance. The
`schedbench` program spawns 4 CPU-bound processes (busy-loop for 100 ticks) and 4
I/O-bound processes (sleep 200 times), collects their PIDs via pipes, then reports
turnaround time, response time, and total wait time per process class.*

```
Test                      | Round-Robin | MLFQ  | Delta
--------------------------|-------------|-------|-------
I/O response time (ticks) |     TBD     |  TBD  |  TBD
CPU turnaround (ticks)    |     TBD     |  TBD  |  TBD
I/O total wait (ticks)    |     TBD     |  TBD  |  TBD
CPU total wait (ticks)    |     TBD     |  TBD  |  TBD
```

---

## What I'd Do Differently / What's Next

The most significant limitation in the current implementation is a scheduler gaming
vulnerability that is subtle enough to deserve a precise description.

A process that calls `yield()` — which is a syscall — at the right moment accumulates
only 1 unit of cpu_time from the syscall entry, not 10 from a timer interrupt. The
timer fires after the process has already yielded the CPU; its `+10` goes to the next
running process. The exploit loop: yield just before the tick fires, wake up as RUNNABLE
with minimal cpu_time, get rescheduled immediately, repeat.

```
  Timeline of a gaming process:

  tick boundary:   0        10       20       30
                   │        │        │        │
  cpu_time:        1  1  1  1  1  1  1  1  1  1  ...  (stays near 0 forever)
                   ↑        ↑        ↑        ↑
                 yield    yield    yield    yield   ← called just before each tick
                 +1 only  +1 only  +1 only  +1 only

  Honest process:
  cpu_time:        0       10       20       30  ...  → demoted at allotment
                            ↑        ↑        ↑
                          timer    timer    timer
                          +10      +10      +10
```

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
