# Kernel Threads in xv6: Adding True Parallelism to a Teaching OS

*Full implementation on [GitHub](https://github.com/Amogh-Nivaskar/xv6-riscv/tree/kernel-threads).*

---

## The Problem


xv6 boots on 3 CPUs. Our prime-counting benchmark takes ~55 million cycles. Two out of three cores sit completely idle during that entire run !!

This is not a quirk — it's the design. In vanilla xv6, every process is a single thread of execution. The scheduler can distribute *different* processes across different CPUs, but a single process never leaves its one assigned core. No matter how CPU-hungry your code is, it gets exactly one core.

### Why does this matter ?

Lets say you have a function that counts all the primes up to 200,000. It's purely CPU-bound — no IO, no blocking, just arithmetic. On a 3-core machine, the obvious thing is to split the range across 3 workers and run them simultaneously on all 3 cores. On a multi-threaded kernel, that's exactly what happens. On vanilla xv6, you get one core doing all the work serially. The other two cores are spinning in the scheduler loop, waiting for a different process to schedule.

The hardware is already parallel. QEMU boots 3 vCPUs. The kernel uses them. But within one program's address space, there's no concept of "run this on CPU 1 and that on CPU 2 at the same time."

---

## The Naive Solution and Why It Fails

The obvious workaround is `fork()`. Split the work across two child processes, communicate via pipes, sum at the end:

```c
if (fork() == 0) {
    uint64 result = count_range(N/2, N);
    write(pipefd[1], &result, sizeof(result));
    exit(0);
}
uint64 local = count_range(2, N/2);
uint64 child_result;
read(pipefd[0], &child_result, sizeof(child_result));
wait(0);
```

This does run the two halves on different CPUs. But it breaks the moment you want shared state.

The key thing that doesn't work: our benchmark uses a shared `partial[]` array. Each worker writes its count to `partial[id]` and the main thread sums them at the end. With `fork()`, parent and child have separate address spaces. The child's write to `partial[1]` is invisible to the parent. There is no shared `partial[]`. You have to serialize everything through the pipe.

That's fine for this benchmark — the result is one `uint64`. But it fundamentally can't work for anything that needs a live shared pointer: a shared counter, a work queue, a lock-protected data structure. With separate address spaces, any shared pointer is meaningless after `fork()`.

Even for simple result sharing, the cost adds up. A pipe round-trip in xv6 is 2–3 context switches plus a kernel buffer copy. For fine-grained work sharing, this serializes exactly what you wanted to parallelize.

What we really need is for multiple threads to run in the same address space simultaneously — sharing the heap, sharing the page table, sharing open files — while each running independently on its own core.

---

## Design Decisions

### 1. Split `struct proc` into `struct thread` + `struct family_shared`

The first question is what the data structure looks like. The obvious approach is to add a `thread_id` field to `struct proc` and clone the whole thing for each thread.

The problem with that is you can't share one `proc` instance across CPUs. Each CPU needs its own execution context: its own register save area (trapframe), its own kernel stack, its own scheduler state. You'd need to lock individual fields at interrupt frequency, which is impractical.

So we split on the per-thread vs per-family axis. Fields that are specific to a single thread's execution — `state`, `chan`, `killed`, `xstate`, `context`, `trapframe`, `kstack_index`, `slot_index` — stay in `struct thread`. Fields shared by all threads in a family — `pagetable`, `sz`, `ofile[]`, `cwd`, `vmas[]`, `tcount`, `slot_tracking[]` — move to `struct family_shared`. Every thread carries a `family` pointer to the same shared struct.

```c
struct thread {
  struct spinlock lock;
  enum threadstate state;
  void *chan;
  int killed;
  int xstate;
  int tid;
  int kstack_index;
  int slot_index;
  struct trapframe *trapframe;
  struct context context;
  struct family_shared *family;
  struct thread *next;
};

struct family_shared {
  struct sleeplock sleeplk;
  struct spinlock spinlk;
  pagetable_t pagetable;
  uint64 sz;
  struct file *ofile[NOFILE];
  struct inode *cwd;
  struct vma vmas[NVMA];
  int tcount;
  int no_clone;
  int slot_tracking[NFAMILY_THREADS];
  int xstate;
  // ...
};
```

The tradeoff: every shared-resource access now goes through an extra pointer dereference via `t->family`. That's a fine price.

### 2. Sleeplock vs spinlock for shared family fields

We considered protecting everything in `family_shared` with a single spinlock for simplicity.

The problem is that page table operations can trigger disk IO (lazy ELF loading via VMAs). A spinlock disables interrupts. If a thread holds a spinlock and goes to sleep waiting for disk IO, the interrupt that would wake it up can never be delivered — deadlock.

So we use two locks on the family struct. `sleeplk` (a sleep lock) protects `pagetable`, `ofile`, `cwd`, and `vmas` — anything that can block. `spinlk` (a spinlock) protects `tcount`, `slot_tracking`, and `no_clone` — short in-memory updates that never block.

### 3. Fixed formula-based slot layout

Each thread needs its own user stack and trapframe in the shared address space. The question is how to assign them virtual addresses.

We considered a find-first-fit allocator in the free virtual address space. We rejected it because `trampoline.S` runs at the moment a trap fires — before any kernel data structure is accessible — and it needs to find the trapframe VA using only the contents of a single register. There's nowhere to store a dynamic lookup table at that point.

So we use a fixed formula. Each thread gets a **slot**: one trapframe page + one stack page + one guard page, allocated downward from the top of user address space.

```
MAXVA
+-------------------------------+
|          Trampoline           |  1 page
+-------------------------------+
|        Trapframe #0           |  1 page  ← slot 0
+-------------------------------+
|       User Stack #0 ↓         |  1 page
+-------------------------------+
|         Guard Page            |  1 page
+-------------------------------+
|        Trapframe #1           |  1 page  ← slot 1
+-------------------------------+
|       User Stack #1 ↓         |  1 page
+-------------------------------+
|         Guard Page            |  1 page
+-------------------------------+
           ...
+-------------------------------+  ← HEAP_RESERVE boundary
|            Heap ↑             |
+-------------------------------+
```

Given a thread's `slot_index`, the addresses follow directly:

```c
slot_base(i)      = MAXVA - 4*PGSIZE - (i * 3 * PGSIZE)
trapframe_base(i) = slot_base(i) + 2*PGSIZE
ustack_top(i)     = slot_base(i) + 2*PGSIZE   // same address, different perspective
```

A `slot_tracking[]` bitmap in `family_shared` records which slots are occupied. We also reserve `HEAP_RESERVE_PAGES` of address space above the heap so thread slot creation can't starve future heap growth.

The tradeoff: the maximum threads per family is bounded by virtual address budget (`NFAMILY_THREADS = 8`). That's fine for our purposes.

### 4. `exec()` kills siblings after ELF parsing, not before

When a thread calls `exec()`, it needs to replace the family's entire address space with a new program. That means killing all sibling threads first.

The tempting approach is to kill siblings immediately, then do the ELF parsing. We rejected this because if `exec()` fails partway through — bad ELF, OOM during page allocation — your siblings are already dead with no way to undo it. The process is now in an unrecoverable state.

So we do all the failure-prone work first: parse the ELF, allocate the new page table, copy the arguments. Only after everything succeeds do we set `no_clone = 1` and kill the siblings. If `exec()` takes the `bad:` path, siblings are still alive and the process is in exactly the state it was before the call.

---

## The Implementation

### The per-thread trapframe problem

Every trap entry goes through `trampoline.S`. The original code has two instructions at the very top of `uservec`:

```asm
csrw sscratch, a0      # save user a0 (we're about to clobber it)
li a0, TRAPFRAME       # load hardcoded trapframe VA into a0
```

From this point, `a0` holds the trapframe base address, and `uservec` saves all user registers into it. The rest of the trampoline is unchanged.

The problem: `TRAPFRAME` is a hardcoded address. There's only one. But now, each thread has its own trapframe at a different slot-derived VA. At the moment `uservec` fires, all we have is the user register set and `sscratch`. There's no safe way to do a memory lookup to find the current thread's trapframe VA.

### The csrrw fix

We use a two-part protocol.

**Part 1:** `prepare_return()` (which runs before every return to user space) writes the current thread's trapframe VA into `sscratch`:

```c
w_sscratch(TRAPFRAME_BASE(mythread()->slot_index));
```

**Part 2:** `uservec` replaces those two instructions with one atomic swap:

```asm
csrrw a0, sscratch, a0
```

`csrrw rd, csr, rs` reads `csr` into `rd` and writes `rs` into `csr` in a single instruction. After it executes: `a0` holds the trapframe VA (written there by `prepare_return()`), and `sscratch` holds the original user `a0`. The rest of `uservec` saves all registers into the trapframe using `a0` as the base — completely unchanged from the original.

The two-instruction original needed to save `a0` *before* loading `TRAPFRAME` so it wouldn't be overwritten. `csrrw` does both in one instruction with no temporaries.

### The backward-compatible edge case

Slot 0's trapframe is at `MAXVA - 4*PGSIZE + 2*PGSIZE = MAXVA - 2*PGSIZE` — exactly where the original hardcoded `TRAPFRAME` was. Single-threaded programs hit the same VA as before. The change is backward-compatible at the virtual address level.

### Dual xstate: where the exit status lives

When a thread calls `exit_thread(status)`, it decrements the family's `tcount` and checks if it's the last one.

The exit status needs to end up somewhere that survives until whoever is waiting reads it. The problem: by the time `kwait()` reads the exit status, the exiting thread's struct may already be freed.

So we store it in two different places depending on which case we're in:

```c
acquire(&f->spinlk);
f->tcount--;
int thread_count = f->tcount;
release(&f->spinlk);

if (thread_count == 0) {
    // Last thread: store on the family, which outlives all threads
    // (also clean up external resources, reparent children, wake parent)
    f->xstate = status;
    td->state = ZOMBIE;
} else {
    // Not last: store on the thread struct — join() reads it directly
    // from the ZOMBIE thread before calling free_thread()
    wakeup(f);
    td->xstate = status;
    td->state = ZOMBIE;
}
sched();
```

`join()` reads from the thread struct (`tt->xstate`) because the ZOMBIE thread is still alive when it's called. `wait()` reads from the family struct (`f->xstate`) because the threads may already be freed by the time `kwait()` runs.

---

## Benchmarks

### Methodology

Getting clean parallel measurements on QEMU is trickier than it sounds. There are a few things we had to get right.

**Platform:** QEMU 8.2, `-machine virt -smp 3 -m 128M`  
**Accelerator:** `-accel tcg,thread=multi` (MTTCG)  
**Measurement:** `rdcycle` CSR, user-mode access enabled via `scounteren`  
**Workload:** Count primes in [2, 200,000], divided into equal contiguous ranges per thread  
**Warmup:** One throwaway `bench(1)` before timed runs  

**QEMU MTTCG**: By default, QEMU's TCG emulator serializes all vCPUs onto a single host thread. Adding `-accel tcg,thread=multi` maps each vCPU to a real host OS thread. When two vCPUs are both `RUNNING`, they genuinely execute in parallel on different host cores.

**RR scheduler**: With MLFQ (our default), idle CPUs execute `wfi` (Wait-For-Interrupt). WFI halts the CPU until the next timer interrupt, which fires roughly every ~100ms. So when you clone a sibling thread, the idle CPU won't pick it up for up to 100ms — making the "parallel" run look almost identical to serial. We call `setscheduler(0)` at the start of the benchmark to switch to Round Robin, where idle CPUs spin continuously. With RR, an idle CPU sees a newly `RUNNABLE` thread within a few thousand cycles.

**The rdcycle migration bug**: `rdcycle` reads the cycle counter of the *current* vCPU. With MTTCG, each vCPU has an independent counter with a different base. If `join()` inside the timed region causes the main thread to sleep and wake on a *different* vCPU, `t1 - t0` wraps to UINT64_MAX. The fix: each sibling sets a `done[i]` flag right before `exit_thread()`, and the main thread spin-waits on those flags before reading `t1`. This keeps the main thread awake and on the same vCPU the whole time. `join()` is called after `t1` is recorded, outside the timing window, purely for TCB cleanup.

### Results

```
Configuration         | Cycles  | vs 1T
----------------------|---------|-------
1 thread              | ~55M    | 1.0x
2 threads (MTTCG+RR)  | ~35M    | 1.5x
3 threads (MTTCG+RR)  | ~25M    | 2.1x
```

The speedup is real but below the theoretical 2x/3x. QEMU's software memory coherence tracking adds overhead that real hardware doesn't have, and timer interrupt handling briefly serializes CPUs when it fires. On real RISC-V hardware with 3 physical cores you'd expect something much closer to the ideal.

The correctness check — all three configurations finding exactly 17,984 primes — verifies that shared address space access across threads is race-free.

### The counterintuitive 3T result

3T (2.1x) shows better-than-proportional improvement over 2T (1.5x). With 2 threads, CPU 2 sits idle spinning in the scheduler. QEMU's software memory coherence tracking picks up cache-line writes from that spinning CPU and adds overhead to the two active ones. With 3 threads, all CPUs are doing real work. There's no idle CPU generating coherence noise. The benchmark actually runs cleaner with all cores occupied.

---

## What We'd Do Differently

### The tcount race bug

`alloc_family()` originally initialized `tcount = 1` as a sentinel, with `kclone()` incrementing it for additional threads.

The bug: between `alloc_family()` inserting the family into the FCB and `alloc_thread()` being called, there's a window where `tcount == 0` and no threads exist yet. If `kwait()` runs during this window, it sees `tcount == 0` with no ZOMBIE threads and concludes the family is dead — freeing it while `fork()` is still setting it up.

The fix: move the increment into `alloc_thread()` and add a `found_any` guard in `kwait()`. A legitimately dead family always has at least one ZOMBIE thread (the last thread sets itself ZOMBIE before decrementing `tcount`). The zero-thread case is exclusively the setup window. So if `kwait()` finds a family with no threads at all, it sleeps and retries rather than freeing it.

```c
int found_any = 0;
for (struct thread *tt = init_thread; tt != NULL; tt = tt->next) {
    if (tt->family == ff) {
        found_any = 1;      // any thread in this family, regardless of state
        acquire(&tt->lock);
        if (tt->state != ZOMBIE) ready = 0;
        release(&tt->lock);
    }
}
// !found_any means the setup window — sleep and retry
if (!found_any || !ready) { sleep(...); }
```

This kind of bug is subtle because the race window is tiny and you only see it under specific scheduling patterns.

### The sleeplock bottleneck

The family sleeplock (`sleeplk`) serializes all page table access — even reads. Two threads doing concurrent `read()` syscalls on separate file descriptors still serialize on the page table lock. A read-write lock would allow true parallel read access, which is the common case. That's the most concrete performance improvement left on the table.

### What's next

User-space threading primitives. The `clone`/`exit_thread`/`join` kernel API is a foundation, not a user API. Mutexes and condition variables built on top of it would make the system actually usable for real concurrent programs.
