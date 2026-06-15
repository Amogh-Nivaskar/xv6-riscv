# Kernel Threads xv6 Extension Design


## Contents
1. Overview — A brief summary of what you're building and why. What problem does it solve?
2. Goals and Non-Goals — What is explicitly in scope and what are you deliberately leaving out.
3. Background — Relevant context about the existing system. In your case, how xv6 currently works that is relevant to your changes.
4. Design — The meat of the document. Broken into subsections covering each major component of your design. Data structures, algorithms, system call interfaces etc.
5. Alternatives Considered — Design decisions where you had multiple options and why you chose what you did.
6. Open Questions — Things you haven't fully resolved yet.
7. Future Work — Things explicitly out of scope now but worth considering later.
---

## Overview

Vanilla xv6 doesn't support multi-threading. Multi-threading allows for a program to have true parallel execution in multi-core processors. 

### Why is this important ?  
Lets say you have a single process running on a multi-core CPU that needs to run two functions funcA() and funcB(), that are independent of each other. On a single-threaded kernel like xv6, the CPU will execute the functions serially, meaning one after the other, on a single core. While this does work, you are wasting the compute of the other cores !! This is highly inefficient. A multi-threaded kernel, on the other hand, will allow both the functions to run in parallel on 2 different cores. This will obviously make your program run much faster. 

### How does a multi-threaded kernel work ?  
The basic idea, is to create a new fundamental mode of execution called `Threads` (or `Tasks` like in Linux) rather than a `Process`. It can be thought of as a Process being made up of multiple Threads. The reason that multiple threads can run in parallel is that a family of threads shares the same Address Space, but has its own independent execution context. Meaning, it can run independently on a core, as if it were its own process. But these threads can also interact with each other, as they share memory and resources.

### How do we achieve this ?  
We need to redefine the `struct proc` into a `struct thread` which will have the independent fields needed for a single threads execution context such as thread ID, instruction pointer, user stack, kernel stack, state, family (a pointer to the shared struct) etc. We also create a `struct thread_family_shared` which contains the shared resources between a family of threads such as the page table, the open files, the current working directory inode and the VMAs list (it is used for lazy loading the ELF pages of the program, and hence needs to be in the shared fields). As mentioned above, each thread in a family has a pointer to the same shared struct. We also need to change the virtual memory layout to accommodate a user stack for each thread within a single Address Space. 

We also need to add two new userspace APIs:
- `int clone(void (*fn)(void *), void *arg);`
- `void exit_thread(void);`

`clone()` creates a new child thread from the currently running thread. The caller provides a function pointer indicating where the new thread should begin execution, along with the argument to pass to that function. `exit_thread()` terminates the calling thread.

We also need to modify the behavior of some existing system calls:

- `fork()` should copy only the calling thread into the child process, and not any of its other family threads.
- `exec()` should terminate the rest of the family threads before replacing the calling thread's full thread family with the new program image.

## Goals

1. A family of threads should properly share the address space and resources, while each having independent execution context.
2. Userspace Virtual Memory Layout needs to be modified to accommodate dynamic creation and destruction of per-thread user stack with guard pages.
3. Support creation and destruction of per-thread kernel stack in the Kernel Virtual Address Space.
4. A thread can be safely created only if all of the following conditions are satisfied:
   - Free physical pages are available for the thread's user stack (ustack).
   - Free physical pages are available for the thread's kernel stack (kstack).
   - A contiguous region of unused virtual address space exists for the user stack and its guard page.
   - The selected stack region does not overlap with the process heap.
5. The shared family struct should be properly protected for concurrent safety.
6. Threads should exit cleanly, freeing their resources without leaking memory
7. The Thread Control Block, should be modified to support dynamic creation and destruction of threads, with concurrent safety.
8. Create new syscall `clone()` to create a new child thread.
9. Create new syscall `exit_thread()` to exit the calling thread.
10. Existing syscalls - `fork()` and `exec()` need to be modified to handle multi-threaded behaviour.
11. Scheduler should be modfied to safely find thread to run, and change its state, while achieving maximum possible concurrency.

## Non-goals

1. We will not implement a production-grade concurrent scheduler. A simple global list lock with per-thread locks is sufficient for our purposes.
2. We will not implement a production-grade dynamic data structure for the TCB. A simple linked list with a global lock will be used despite its lock contention limitations.
3. The stacks will be fixed to 1 page (4 KB) in size and will not grow, because this adds complexity to the algorithm which decides the stack placement in address space.
4. We will not implement a flexible virtual memory allocator. Stack placement is formula-based using a fixed tracking array, and heap growth is managed via the existing sbrk() mechanism. Stack and heap regions contend over the available virtual address space. As a result the maximum number of threads per family is bounded by available virtual address space and physical memory.
6. There will be no user managed stacks for threads created using `clone()`, as there is no need for it, due to our stacks being fixed in size and also the kernel has the full knowledge of where to place stacks via the tracking array, so there's no need to burden the user with stack management.

## Background

### Process
The Process, defined by `struct proc` is the execution state of a program. It contains the process's page table, state, trapframe, context, open files list, current working directory inode, VMAs list etc.

### Process Control Block (PCB)
The PCB is defined as `struct proc[NPROC]`, which is a fixed size array. Meaning that regardless of how many processes are running, we have to keep the whole array initialized. Also, you can't have more than NPROC processes running at any given time, due to the fixed length. This fixed array approach motivates our switch to a dynamic linked list in the new design.

### Process Lifecycle
The first process is created by `userinit()` and is saved as a global variable as `initproc` which has a PID = 1. A new process can be spun up by calling `fork()` which creates a new child process by copying the full address space of the parent process. `exec()` can be called by a process to change the program running on it thereby overriding its entire address space. A parent process can call `wait()`, to wait for a child process to finish execution and get the child's PID.  
The mechanism of deleting a process is not this straight forward. The reason for that is that a process cannot delete itself, due to that fact that deleting a process includes freeing the kernel stack. But you can't free the kernel stack while you are still executing code on that stack. So we need to work around this.  
When ever a process wants to delete itself, it calls `exit()` and if it wants to delete another process, it calls `kill(pid)` with the target process's PID. 
`kill(pid)` loops over the PCB to find the process with the given PID and sets its killed flag `p->killed = 1`. Now, whenever this process traps into kernel mode, and if its `killed` flag is set, then it calls `exit()`.  
`exit()` cleans up all the external resources like the open files, the current working directory inode (ideally the VMAs clean up should also happen here as it is also an external resource, but is kept in `freeproc()`). It changes the parent of all of this process's children to `initproc` so that they don't become orphaned once this process is deleted. Then it wakes up its parent process `wakeup(p->parent)`, as it maybe sleeping in `wait()`. It then changes the state of the process to `ZOMBIE` and jumps into the scheduler.
The process's parent wakes up from its sleep from inside `wait()` and for each of its child (including the earlier process) that has a state of `ZOMBIE`, it calls `freeproc()`, which frees the child process's page table and heap. It cleans the state of the child process and marks state as `UNUSED`, so that it can be picked up to be used for another process.  
Since `struct proc` is statically allocated in a fixed array, `freeproc()` does not free the struct itself — it simply marks the slot as `UNUSED` so it can be reused for a future process. Similarly, kernel stacks are pre-allocated at boot time and are never freed, just reused. This is in contrast to our new design where both thread structs and kernel stacks are dynamically allocated and must be explicitly freed.


### Kernel Stacks
Each process has its own kernel stack. When a process enters kernel mode (for eg: during a syscall), it switches its SP (stack pointer) from the user stack (ustack) to the kernel stack (kstack).  
When the kernel boots, the kstacks for all the `NPROC` processes are initialized in the kernel page table. `procinit()` initializes the kstack pointers for all `NPROC` processes using `KSTACK(i)`, which is a fixed formula to compute the kstack address via the process index `i`. The actually kstack allocation for each process is done in  `proc_mapstacks()`, which allocates a page from free-list using `kalloc()` and maps it to kernel page table at the Virtual Address (VA) calculated using `KSTACK(i)`.

### Kernel Virtual Address Space

```text
MAXVA
+-------------------------------+
|          Trampoline           |  1 page
+-------------------------------+
|      Kernel Stack #0 ↓        |  1 page
+-------------------------------+
|         Guard Page            |  1 page
+-------------------------------+
|      Kernel Stack #1 ↓        |  1 page
+-------------------------------+
|         Guard Page            |  1 page
+-------------------------------+
|              ...              |
+-------------------------------+
|   Kernel Stack #(NPROC-1) ↓   |  1 page
+-------------------------------+
|         Guard Page            |  1 page
+-------------------------------+
|                               |
|                               |
|                               |
|      Unused Virtual Space     |
|                               |
|                               |
|                               |
+-------------------------------+  <-- PHYSTOP
|                               |
|         Free Memory           |
|      (managed by kalloc)      |
|                               |
+-------------------------------+  <-- end
|                               |
|      Kernel Text/Data/BSS     |
|                               |
+-------------------------------+  <-- KERNBASE
|             PLIC              |
+-------------------------------+
|            VIRTIO             |
+-------------------------------+
|            UART0              |
+-------------------------------+
0x0000000000000000
```
The xv6 kernel maintains a single global page table that is shared by all processes and threads. At the lower end of the virtual address space, device memory regions such as UART0, VIRTIO, and the PLIC are mapped at fixed addresses. Beginning at `KERNBASE`, the kernel establishes an identity mapping of physical memory, where virtual addresses directly correspond to physical addresses.
The kernel stacks and trampoline page are exceptions and are mapped separately near `MAXVA`.

The region from `KERNBASE` to `end` contains the kernel image, including the kernel's text, data, and BSS sections. The symbol `end` marks the first address immediately following the kernel image. All physical memory between `end` and `PHYSTOP` is initially free and is added to the kernel's page allocator during boot. This region serves as the source of physical pages used throughout the system for user memory, page tables, kernel data structures, and other dynamic allocations.

Near the upper end of the virtual address space, xv6 maps a dedicated kernel stack for each process. Each kernel stack occupies a single page and is protected by an adjacent unmapped guard page that detects stack overflows. Finally, the trampoline page is mapped at the highest virtual address and contains the code used during transitions between user mode and kernel mode.


### Process Context - `struct context`
Contains the callee-saved registers (s0-s11, ra, sp) that represent a process's kernel execution context. These registers are saved during a context switch and restored when the scheduler switches back to the process, allowing kernel execution to resume from where it left off.

### Context Switch - `swtch()`
A context switch takes place, when the kernel calls `swtch(&P1->context, &P2->context)`, to switch from current running process (P1) to the next running process (P2), by saving all the callee-saved registers of P1 in the `struct context`  present in its `struct proc` and loading the callee-saved registers of the P2 from its `struct context`. 


### Scheduler
When a trap occurs, we enter the trap handler (`usertrap()`, `kerneltrap()`). If it is a timer interrupt, we call `yield()` to give up this process from this CPU. `yield()` calls `sched()`, which peforms the context switch between the current running process and the scheduler context via `swtch(&p->context, &mycpu()->context)`. After this, the scheduler process starts running from where it had left off in the `scheduler()` function, which is basically an infinite loop, inside which we use the scheduling algorithm to find the next process to run. Our xv6 supports two scheduling algorithms — Round Robin (RR) and Multi-Level Feedback Queue (MLFQ) — which can be switched between at runtime via a syscall. Once the next process to run in found (np), we change its state to `RUNNING` and switch to its execution using `swtch(&cpu->context, &np->context)`. Now the new process (np) is running on the CPU.

### Process Trapframe - `struct trapframe`
Contains the complete CPU register state of a process in user mode. It is saved when a trap, interrupt, or system call transfers control to the kernel, and restored when the process returns to user mode, allowing execution to resume exactly where it left off.

### User Trap Handler - `usertrap()`
Handles a trap originating from user mode, such as an interrupt, exception, or system call. It is invoked from **trampoline.S** after the full user CPU state has been saved into the process's `trapframe`, and the kernel stack pointer and kernel page table have been restored from the `trapframe`. Depending on the cause of the trap, it either handles the event directly or, in the case of a timer interrupt, calls `yield()` to allow another process to run. Once trap handling is complete, it calls `usertrapret()` to prepare for returning to user mode.

An important point to keep in mind, is that in **trampoline.S** the trapframe address used is hardcoded in the `TRAPFRAME` variable, as each process uses a single trapframe and thus we always keep it just below the Trampoline page i.e at `MAXVA - 2PGSIZE` as can be seen in the [memory layout section](#user-mode-virtual-address-space-memory-layout)

### User Trap Return - `usertrapret()` (or `prepare_return()` in our code)
Prepares a process to return from kernel mode to user mode after a trap has been handled. It stores the kernel state required for future traps in the process's `trapframe`, configures the CPU control registers so that `sret` will return to user mode, switches to the process's user page table, and then transfers control to the **trampoline.S** `userret()` routine, which restores the saved user register state and executes sret.

### User Mode Virtual Address Space Memory Layout

```text
MAXVA
+-------------------------------+
|          Trampoline           |
+-------------------------------+
|           Trapframe           |
+-------------------------------+
|                               |
|         Free Memory           |
|                               |
|              ↓                |
+-------------------------------+
|            Heap               |
|         (grows up ↑)          |
+-------------------------------+
|           Stack               |
|      (grows down ↓)           |
+-------------------------------+
|         Guard Page            |
+-------------------------------+
|         Data + BSS            |
+-------------------------------+
|            Text               |
+-------------------------------+
0x0000000000000000
```

The user address space begins with the program's **Text** and **Data + BSS** sections at the lowest virtual addresses. Immediately above them lies an unmapped **Guard Page**, followed by the process's **Stack** page, which grows downward toward lower addresses. As the stack grows downwards, the guard page catches any overflows. The **Heap** is located above the stack and grows upward as memory is allocated through `sbrk()`. The region between the heap and the upper mappings remains unmapped and serves as free virtual address space (**Free Memory**) available for future heap growth. At the highest virtual addresses, xv6 maps the **Trapframe** and **Trampoline** pages, which are used to save user register state during traps and to facilitate transitions between user and kernel mode.


## Design

### 1. New Thread Data Structure 

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
  
  struct thread_family_shared *family;
  struct thread *next;

  char name[16];              
};

struct spinlock thread_list_lock;

struct thread *init_thread;

```
The new `struct thread` contains only the per-thread state required for scheduling, trap handling, and lifecycle management. Unlike the original `struct proc`, it does not contain resources that are shared across all threads in a thread family, such as the address space, open file table, current working directory etc. These shared fields have been moved to a separate shared data structure, to which the `family` field is a pointer to. This is described in more detail in the next section.

`slot_index` is the index that this thread's slot has occupied in the `slot_tracking` array (covered in detail in the subsection 2). It has an uninitialized value of `-1`.

`kstack_index` is the index that this thread's kernel stack has occupied in the global `kstack_tracking` array (covered in detail in the subsection 6). It has an uninitialized value of `-1`.

Note that we have removed the `parent` field which was present in the `struct proc`. Thats because all threads belonging to a family will be sibling threads.

The structure is protected by a **spin lock** rather than a sleep lock. The scheduler frequently accesses and updates thread state while selecting runnable threads. If contention on the lock caused the scheduler to sleep, scheduling itself could be blocked waiting for access to thread state, creating the possibility of deadlock. Since these operations involve only short in-memory updates, a spin lock is a more appropriate choice.


Earlier we had a fixed array as the PCB, but now we move to a **Linked List** for the Thread Control Block (TCB). We choose this approach as this allows us to dynamically allocate threads without any strict upper bound imposed by the list. The downside is that to the traversing to a specific thread takes `O(n)` time (vs `O(1)` in earlier approach via array index). But this is fine as the scheduler loops over all the threads anyway.

The `next` field is a pointer to the next node in the TCB. 

`init_thread` is the head of the Linked List and also the first thread created in the system by `userinit()`. It is great as a head as it will never exit.

`thread_list_lock` is a global lock for this list's structure. Hence, when we loop over the list or add or remove threads, we need to acquire this lock. On the other hand, the per-thread lock in `thread` is used when modifying the thread itself. To avoid deadlocks, you acquire the `thread_list_lock` first and then find the thread, then acquire a lock on the thread, release `thread_list_lock`, make the changes you want on the thread and then release the lock on the thread.


### 2. New Thread Family Shared Data Structure

```c
struct thread_family_shared {
  struct sleeplock sleeplk;
  struct spinlock spinlk;

  pagetable_t pagetable;  
  uint64 sz;

  struct file *ofile[NOFILE];
  struct inode *cwd;
  struct vma vmas[NVMA];

  struct thread_family_shared *parent_family;
  struct thread_family_shared *next;
  int fid;

  int tcount;
  int no_clone;
  int slot_tracking[NFAMILY_THREADS];
  uint64 heap_reserve;
};

struct spinlock family_list_lock;

struct family_thread_shared *init_family;

```
The new `struct thread_family_shared` contains the fields common to a thread family such as the address space via the page table, the heap size (i.e. the top of the heap), the list of open files, the inode pointer to the current working directory and the list of VMAs, needed to lazy load the ELF segments of the program.

[**NOTE:** The sleep lock should be held wherever any kind of page table access needs to be made. So the existing page table access need to be modified to hold the lock.]

Each thread in the same family have their `family` pointer pointing to the same `struct thread_family_shared` object.

`parent_family` is a pointer to the family of the parent thread that spawned this family.

`fid` is unique identifier for the family. It acts as the `PID` for user programs.

The `slot_tracking` array is a boolean array. If `slot_tracking[slot_index] == 1` then the thread slot at index `slot_index` is occupied, else it is unoccupied. This array is useful when assigning a **thread slot** memory region to a new thread. For a given thread's slot index, we can derive formulas for calculating the address of the thread slot's base, the top of the user stack and the base of the trapframe. These formulas are mentioned in the **Virtual Memory Layout** subsection coming up next.

`NFAMILY_THREADS` is the absolute maximum number of threads that you can create in a single thread family. Its limited by the size of the **User Virtual Address Space** as it can accommodate only a limited number of thread slots.

`heap_reserve` is the boundary beyond which thread slots can't be allocated. This will be explained in more detail in the New Memory Layout subsection

The `no_clone` boolean flag, basically doesn't allow for a thread to be cloned (i.e. another thread to be created) if it is True. We need to turn off cloning especillay when we call `exec()` and kill off all the other family threads, and don't want any new threads created and also in `exit()` when we want to kill all the threads in the family and hence don't want any new threads to be created.

The `tcount` field keeps count of the number of threads alive in this family.

Similar to the TCB, we also use a Linked List to store the Family Control Block (FCB).

The `next` field is a pointer to the next family in FCB. `init_family` (`init_thread->family` = `init_family`) is used as a head of this list as it never exits.

We use a **sleep lock** (`sleeplk`) to protect the shared thread-family state of `pagetable`, `ofile`, `cwd` and `vmas`. Operations on shared resources such as the page table, open file table, current working directory, and VMAs may involve acquiring inode locks, waiting for disk I/O or other actions that can cause the calling thread to sleep. A spin lock is unsuitable here because xv6 disables interrupts while a spinlock is held. If a thread holding a spinlock goes to sleep waiting for an event that depends on an interrupt (such as disk I/O completion), the interrupt cannot be delivered, resulting in a deadlock. A sleep lock avoids this issue by allowing the thread to block and yield the CPU while waiting for the resource to become available.

We use a **spin lock** (`spinlk`) to protect the thread count (`tcount`) and slot tracking array (`slot_tracking`) as their operations have short critical sections and aren't dependent on interrupts.

`family_list_lock` is a global lock for this list's structure. Hence, when we loop over the list or add or remove families, we need to acquire this lock.


### 3. New Memory Layout 

```text
MAXVA
+-------------------------------+
|          Trampoline           |  1 page
+-------------------------------+
|        Trapframe #0           |  1 page
+-------------------------------+
|       User Stack #0 ↓         |  1 page
+-------------------------------+
|         Guard Page            |  1 page
+-------------------------------+
|        Trapframe #1           |  1 page
+-------------------------------+
|       User Stack #1 ↓         |  1 page
+-------------------------------+
|         Guard Page            |  1 page
+-------------------------------+
|        Trapframe #2           |  1 page
+-------------------------------+
|       User Stack #2 ↓         |  1 page
+-------------------------------+
|         Guard Page            |  1 page
+-------------------------------+  ← Additional thread slots
|                               |     may be allocated here
|         Free Memory           |
|                               |
+-------------------------------+  <-- HEAP_RESERVE
|                               |
|         Free Memory           |
|                               |
+-------------------------------+
|             Heap              |  1 page (initially)
|               ↑               |
+-------------------------------+
|          Data + BSS           |
+-------------------------------+
|             Text              |
+-------------------------------+
0x0000000000000000

```

In the new memory layout, thread resources are allocated from the high end of the user address space and extend downward toward lower virtual addresses. Each thread is assigned a dedicated **Thread Slot**, which consists of a one-page trapframe, a one-page user stack, and a one-page guard page, as each thread needs its own trapframe for storing user-mode CPU state before a trap and user stack for independent execution:

```text
    Thread Slot
+------------------+
|    Trapframe     |
+------------------+
|    User Stack    |
+------------------+
|    Guard Page    |
+------------------+
```

The trapframe stores the complete user register state of the thread during trap handling. The user stack provides the thread's execution stack, while the guard page remains unmapped and serves as a protection mechanism against stack overflows. Since thread slot allocation proceeds downward through the free address space, newly created threads receive thread slots at progressively lower virtual addresses.

Also, now we can derive formulas for calculating the address of the thread slot's base, the top of the user stack and the base of the trapframe, given a thread's slot index.

```c
slot_base(i)    = MAXVA - 4*PGSIZE - (i * 3 * PGSIZE)
ustack_top(i)   = slot_base(i) + 2*PGSIZE   // initial SP value
trapframe_base(i) = slot_base(i) + 2*PGSIZE   // start of trapframe page
```

In our notation, `slot_base` is the **bottom** of the guard-page, `ustack_top` is the **top** of the user stack  and `trapframe_base` is the **bottom** of the trapframe. `ustack_top` and `trapframe_base` evaluate to the same address. This is the boundary between the stack page and the trapframe page. From the stack's perspective it is the initial SP value; from the trapframe's perspective it is the start of the trapframe page. The allocation of the thread slot is covered in the next section.

The heap starts with one page and grows upward via `sbrk()` while thread stack allocation progresses downward, both regions compete for the same free address space. A naïve allocation strategy would allow thread creation to consume all available free space, potentially preventing future heap growth. To avoid this situation, the design introduces a reservation boundary, `HEAP_RESERVE`, within the free address space. New thread stacks may only be allocated above this boundary, ensuring that a portion of the address space remains available for future heap expansion.

`HEAP_RESERVE` is not a hard limit on heap growth. The boundary only constrains stack allocation. If the heap requires additional pages and free space exists above the reservation boundary, it is permitted to grow beyond `HEAP_RESERVE`. In effect, the reservation acts as a one-way constraint: thread stacks may not cross below the boundary, but the heap may grow beyond it when necessary. The `HEAP_RESERVE` address is stored in the `struct thread_family_shared` in field `heap_reserve`. It is computed in `exec()` using the following formula - 
```c
heap_reserve = initial_sz + (HEAP_RESERVE_PAGES * PGSIZE)
```
`HEAP_RESERVE_PAGES` are the number of pages we want to reserve for the heap. For our implementation, we will go with `HEAP_RESERVE_PAGES = 4`. The `initial_sz` is the `sz` value in `exec()`, which is basically the upper boundary of ELF. 

This approach provides a simple and predictable balance between thread creation and dynamic memory allocation. By reserving address space for future heap growth, the design prevents excessive thread creation from starving the heap while still allowing unused reserved space to be utilized by the heap when required.


### 4. Modified Trap Handling for Per-thread Trapframes

As mentioned in the [User Trap handling subsection](#user-trap-handler---usertrap), earlier we used to only have one trapframe per process and thus the trapframe address was hardcoded in **trampoline.S**

But now, multiple thread's share the same address space and each thread needs its own trapframe to store its kernel execution state.
Hence, we now need to find a way to make the **trampoline.S** aware of where the thread's trapframe is located. 

To do this, we first need to get the `trapframe_base` address, using the formula specified in [New Memory Layout section](#3-new-memory-layout).

Now we need to get the thread's trapframe base VA into sscratch. For this we will need to add two functions - 
```c
static inline void w_sscratch(uint64 x) {
  asm volatile("csrw sscratch, %0" : : "r" (x));
}

static inline uint64 r_sscratch() {
  uint64 x;
  asm volatile("csrr %0, sscratch" : "=r" (x));
  return x;
}
```

Then we modify the `prepare_return()` (earlier `usertrapret()`) to accesses the current thread via `mythread()` and uses `td->slot_index` with the `trapframe_base()` formula to compute the correct trapframe VA before writing it to sscratch with `w_sscratch(trapframe_base)`.

Lastly, in **trampoline.S** we swap these 2 lines  -  
   1. `csrw sscratch, a0` - load the value of register a0 into register sscratch, so that we don't lose the value in a0.
   2. `li a0, TRAPFRAME` - loads the hardcoded trapframe address into register a0
with this -  
`csrrw a0, sscratch, a0` - which does an atomic swap between the values of register a0 and register sscratch.  

After the instruction is executed, a0 now has the trapframe VA and sscratch has the earlier a0's value. From here on out, the trampoline can function just as it did.





### 5. Thread Lifecycle

#### Birth:
The first thread is created by `userinit()` and is saved as a global variable as `init_thread` which has a `TID = 1`. `init_thread` will always be the only thread in its family.

A new thread is created in the family by calling the function `clone()`. It internally calls `alloc_thread()` by passing the calling thread's family to it. `alloc_thread()` initializes a new thread object and also allocates a new thread slot (`alloc_slot()`) and a kernel stack (`alloc_kstack()`). `clone()` also sets up the program counter with a function pointer, from where the thread can start executing.  
So from this, you might have noticed that the `clone()` can only create a thread in the same family.

To create a thread with another family, we call `fork()`. The family created by `fork()` is the child of the calling thread's family and thus the calling thread's family is the parent family. The design of parent-child relation being between families rather than threads is important for other mechanisms. `fork()` calls `alloc_family()` internally and then also calls `alloc_thread()` with the newly created family. `fork()` will use `uvmcopy_thread()` to selectively copy just the calling thread's slot and other necessary resources, rather than copying full address space as the calling thread's address space also has slots of other sibling threads.

`exec()` is used to override the the current family's running program with another one. When a thread calls `exec()`, it marks all the other threads in the family as `killed = 1` and waits for them to exit using `join()`. After all the other family threads have exited, then the address space is replaced.

#### Death
A thread can call `exit_thread()` to delete itself or can call `kill_thread(tid)` to delete a family thread with `TID = tid`. 

When a thread calls `kill_thread(tid)`, it sets `killed = 1` for the target thread. When this thread hits a trap and calls `usertrap()`, it calls `exit_thread()` as its `killed` flag is set. 

In `exit_thread()`, it decrements the thread count of the family. If the thread count of the calling thread's family is 0, i.e. this is the last thread in the family, then it cleans up all of the external resources of the family and calls `reparent()` to reparent all of its children families to `init_thread`'s family and lastly wakes up a thread in the parent family sleeping on the parent family's channel.  
If this is not the last thread in the family, then we just wake up a sibling thread sleeping on the family's channel.
In both the above mentioned cases, we awaken a different thread (either a sibling thread or parent family's thread), so that it can clean up the remaining resources of the exited process.  
It then changes the state of the thread to `ZOMBIE` and then jumps into the scheduler.  

`exit()` is used to delete the full family. Internally, it calls `kill_thread()` on each thread in the family. 

`kill(fid)` is used to delete full family with passed `FID`. Internally, it calls `kill_thread()` for each thread in the target family.


There are two waiting mechanisms - `wait()` and `join()` -

In `wait()` the thread waits for all the threads in a child family to exit. It has a infinite loop, inside which it loops over the children families to find one with no active threads i.e. `tcount == 0`. If it finds one, it calls `free_thread()` on each thread still in `ZOMBIE` state and then calls `free_family()` and return the freed family's `FID`.
If it doesn't find one, it sleeps on a channel of its family, so that it is awoken by a child family's last thread exiting, to go over the loop to clear all resources.

In `join()` the thread waits for a thread in the family to `exit()`. It has a infinite loop, inside which it loops over the sibling threads to find in `ZOMBIE` state. If it finds one, it calls calls `free_thread()` on it and return the freed thread's `TID`.
If it doesn't find one, it sleeps on a channel of its family, so that it is awoken by a sibling thread exiting, to go over the loop to clear all resources.

`free_thread()` internally calls `free_slot()` and `free_kstack()` to free the thread's slot and kernel stack respectively.



### 6. Per-thread Slot handling with Per-thread Slot Tracking array 
The  `clone()` function needs to assign a user stack succesfully to create a new thread. It will call the function whos definition is given below.  `td` is the a pointer to the thread for whom we are allocating a slot. The function will return `0` on success and `-1` on failure.
```c
int alloc_slot(struct thread *td)
```
The `alloc_slot()` function will follow these steps:

1. Create variable - `struct thread_family_shared family = td->family;`
2. Mark a slot in the `slot_tracking` array by doing the following:
   1. Acquire the `spinlk` lock on the `family`. 
   2. Find an empty stack slot by looping over `slot_tracking` and finding first index where `slot_tracking[index] == 0`. Lets call this index `slotIdx`. 
   3. Release the lock. 
   4. If `slotIdx` is found, mark `slot_tracking[slotIdx] = 1`. Else, then go to **Step 10**
3. Mark slot index - `td->slot_index = slotIdx;`
4. Find the `ustack_top` and `trapframe_base` addresses using the above given formula.
5. Make sure the slot is not overlapping with the heap and it is above the `heap_reserve`.  
   ```c
    slot_base > family->sz && slot_base > family->heap_reserve
   ```
   If this condition is not true, go to **Step 10**
6. Allocate the user stack by doing the following:
   1.  Get a page of memory by calling `kalloc()`. If it fails, then go to **Step 10**.
   2.  Acquire the sleep lock on `family`
   3.  Map the page from `ustack_top - PGSIZE` address using `mappages()`. If `mappages()` fails, release the lock and go to **Step 10**.
   4.  Keep the guard page unmapped, so that it can catch any stack overflows. 
   5.  Release the lock.
   6.  Return `0` for success.
7. Allocate the trapframe by doing the following:
   1.  Get a page of memory by calling `kalloc()`. If it fails, then go to **Step 10**.
   2.  Acquire the sleep lock on `family`
   3.  Map the page from `trapframe_base` address using `mappages()`. If `mappages()` fails, release the lock and go to **Step 10**.
   4.  Assign the trapframe base to the thread - `td->trapframe = trapframe_base;`
   5.  Release the lock.
8. Assign stack pointer - `td->trapframe->sp = ustack_top`
9.  Return `0` for success.
10. This is the **clean up** step, in case of failure. 
   1. Free the stack page, if allocated. 
   2. Free the trapframe page, if allocated.
   3. If user stack `mappages()` succeeded before failure, then we need to acquire the sleep lock on `family` and unmap the `ustack` page from the page table using `uvmunmap()` and then release the lock
   4. If traprame `mappages()` succeeded before failure, then we need to acquire the sleep lock on `family` and unmap the `trapframe` page from the page table using `uvmunmap()` and then release the lock
   5. If the `slot_tracking[slotIdx]` is marked, then we acquire the spin lock on `family`, and then we unmark it as `slot_tracking[slotIdx] = 0`.  Now, we release the lock.
   6. If `td->slot_index` is marked, then we unmark it - `td->slot_index = -1`
   7. Return `-1` for failure.

Also, the slot needs to be freed on thread exit.
```c
void free_slot(struct thread *td);
```
For this we follow these steps:
1. Create variable - `struct thread_family_shared family = td->family;`
2. Acquire the sleep lock on `family` 
3. Unmap the stack page from page table, and free the stack page, using `uvmunmap()`.
4. Unmap the trapframe page from page table, and free the stack page, using `uvmunmap()`.
5. Release the lock.
6. Acquire the spin lock on the thread's `family` struct.
7. Mark the slot memory region as free as such - `slot_tracking[td->slot_index] = 0;`.
8. Release the lock
9. Unmark the slot index - `td->slot_index = -1;`.


### 7. Per-thread Kernel Stack handling with Global Kernel Stack Tracking array 
Unlike earlier where we used to allocate all `NPROC` kernel stacks when the kernel boots up, now we don't allocate any kernel stacks at boot time. When a new thread is being created, we allocate a dedicated kernel stack for it. Other than that, the memory layout doesn't change at all. Here also we run into the same problem of needing to track which stack regions are occupied and which aren't and so here we use a global kernel stack tracking array (`kstack_tracking`) similar to the per-thread slot tracking array but this one only tracks kernel stacks, along with a global lock (`kstack_tracking_lock`) to protect it. We also add a spin lock (`kpgtbl_lock`) to protect the kernel page table. Earlier we didn't need one as after the kernel boot, the kernel page table was essentially read-only. That is not the case now and so we do need a lock to protect it.
```c
int kstack_tracking[NTHREADS];

struct spinlock kstack_tracking_lock;

struct spinlock kpgtbl_lock;
```
`NTHREADS` is the absolute maximum number of threads that you can create in the full kernel. Its limited by the size of the **Kernel Virtual Address Space** as it can accommodate only a limited number of kernel stacks.

Since the differences in the user and kernel space memory layout, the formula to find the `kstack_base` address i.e. the address of the lower end of the stack, from its kernel stack tracking array index is as follows: 

```c
kstack_base(i) = MAXVA - 2*PGSIZE - (i * 2 * PGSIZE)
```
The  `clone()` function needs to assign a kernel stack succesfully to create a new thread. It will call the function whos definition is given below. `td` is the new thread being created. The function will return `0` on success and `-1` on failure.
```c
int alloc_kstack(struct thread *td)
```
To achieve this, we will follow these steps:
1. Acquire the global `kstack_tracking_lock` lock. Find an empty stack slot by looping over `kstack_tracking` and finding first index where `kstack_tracking[index] == 0`. Lets call this index `kstackIdx`. If such an index is found, mark `kstack_tracking[kstackIdx] = 1`. Release the lock. If such an index is not found then go to **Step 8**
2. Find the `kstack_base` address using the above given formula and the `kstackIdx`.
3. Assign kernel stack pointer - `td->context.sp = kstack_base + PGSIZE`
4. Allocate the kernel stack by doing the following:
   1. Get a page of memory by calling `kalloc()`. If it fails, then go to **Step 8**. 
   2. Acquire the spin lock on `kpgtbl_lock` and map the page to the `kstack_base` address using `mappages()`. If `mappages()` fails, go to **Step 8**. Keep the guard page unmapped, so that it can catch any stack overflows. 
   3. Release the lock. 
5. Assign kernel stack index - `td->kstack_index = kstackIdx;`.
6. Assign kernel stack pointer - `td->context.sp = kstack_base + PGSIZE` 
7. Return `0` for success.
8. This is the **clean up** step, in case of failure. 
   1. If the `kstackIdx` is marked, then we unmark it by doing the following:
      1. Acquire spin lock `kstack_tracking_lock`
      2. Unmark `kstackIdx` - `kstack_tracking[kstack_index] = 0`
      3. Release the lock
   2. Unassign kernel stack index - `td->kstack_index = -1;`.
   3. If the kernel stack page was allocated, then free it using `kfree()`
   4. If `mappages()` succeeded before failure, then we need to unmap the kernel stack by doing the following:
      1. Acquire the spin lock on `kpgtbl_lock` 
      2. Unmap the `kstack` page from the page table using `uvmunmap()` 
      3. Release the lock
   5. Return `-1` for failure.

Also, the kernel stack needs to be freed on thread exit. 
```c
void free_kstack(struct thread *td)
```
For this we follow these steps:
1. Acquire the spin lock on `kpgtbl_lock` 
2. Unmap the stack page from kernel page table and free the stack page, using `uvmunmap()` called with `do_free = 1`. 
3. Release the lock.
4. Unmark kernel stack tracking array, by doing the following:
   1.  Acquire the spin lock on the `kstack_tracking_lock`.
   2.  Unmark array's index value - `kstack_tracking[td->kstack_index] = 0;`.
   3.  Release the lock
5. Unassign the kernel stack index - `td->kstack_index = -1;`.


### 8. Allocation & Deallocation of a thread

```c
int alloc_thread(struct thread_family_shared *family, struct thread **new_thread)
```
It is used to create a new thread. Here `family` is the pointer to the to be created thread's family and new thread is a pointer to a pointer to the new thread to be created.

This is achieved by doing the following:  
1. Allocate memory for a thread - `*new_thread = (struct thread *)kalloc();`. On failure go to *Step 11*.
2. Zero initialize thread's next pointer - `new_thread->next = NULL;`.
3. Assign the family pointer for child thread - `new_thread->family = family;`
4. Initialize thread's index fields with sentinal values and state -
   1. `new_thread->kstack_index = -1;`
   2. `new_thread->slot_index = -1;`
   3. `new_thread->state = UNUSED;`
5. Allocate thread slot by calling -  `alloc_slot(new_thread)`. On failure go to *Step 11*.
6. Allocate kernel stack by calling - `alloc_kstack(new_thread)` On failure go to *Step 11*.
7. Increment thread count by doing the following:
   1. Acquire the family shared spin lock `spinlk`.
   2. Increment the thread counter - `family->tcount += 1;`.
   3. Release the lock
8. Initialze the **return address** - `new_thread->context.ra = forkret`.
9.  Insert thread in TCB.
    1. If it is the first thread, then assign it to `init_thread`
    2. Else, insert new thread into first position in TCB List
   ```c
   aquire(thread_list_lock);
   if (init_thread == NULL){
      init_thread = new_thread;
   }else{
      struct thread prev_first = init_thread.next;
      init_thread.next = new_thread;
      new_thread.next = prev_first;
   }
   release(thread_list_lock);
   ```
10. Return 0.
11.  This is the **clean up** step, in case of failure:  
   1. If failure is after *Step 1*, then free the thread node memory - `kfree(new_thread);`.
   2. If failure is after *Step 5*, then free the slot - `free_slot(family, new_thread);`.
   3. If failure is after *Step 6*, then free the kernel stack - `free_kstack(new_thread);`.
   4. If failure is after *Step 7* then decrement the thread count by doing the following:
      1. Acquire the family shared spin lock `spinlk`.
      2. Decrement the thread counter - `family->tcount -= 1;`.
      3. Release the lock
   5. Return -1.


```c
void free_thread(struct thread *td)
```
This function is used to free the allocated thread `td`.

This is achieved by doing the following: 
1. Panic if it is the `init_thread`.
   ```c
   if (td == init_thread)
      panic("free_thread: attempting to free init_thread");
   ```
2. Free the slot - `free_slot(td);`.
3. Free the kernel stack - `free_kstack(td);`.
4. Remove the thread's node from TCB by doing the following:
   1. Acquire TCB lock - `aquire(thread_list_lock);`
   2. Loop over the TCB to find the thread's node (`target`) and also the node just before it (`prev_target`):
      ```c
      struct thread *target = init_thread->next;
      struct thread *prev_target = init_thread;
      
      while (target != NULL){
         if (target->tid == td.tid) break;
         target = target->next;
         prev_target = prev_target->next;
      }

      if (target == NULL) {
         panic("Thread node not found in TCB");
      }
      ```
   3. Remove the thread's node from TCB:
      ```c
      prev_target->next = target->next;
      ``` 
   4. Release the lock.
5. Free the thread's node's memory - `kfree(target);`.
   


### 9. Allocation & Deallocation of a thread's family

```c
struct thread_family_shared* alloc_family()
```
This function allocates a family and returns a pointer to it.

This is achieved by doing the following: 
1. Allocate memory for shared family object - `struct thread_family_shared *family = kalloc();`. On failure go to *Step 6*.
2. Assign calling thread's family as parent - `family->parent = mythread()->family`
3. Initialize empty user page table - `family->pagetable = thread_pagetable()`. On failure go to *Step 6*.
4. Zero initialze some family fields - 
   ```c
   family->slot_tracking = {0};
   family->no_clone = 0;
   family->tcount = 0;
   family->sz = 0;
   ```
5. Insert family in FCB.
    1. If it is the first family, then assign it to `init_family`
    2. Else, insert new family into first position in FCB List
   ```c
   aquire(family_list_lock);
   if (init_family == NULL){
      init_family = family;
   }else{
      struct thread_family_shared prev_first = init_family->next;
      init_family->next = family;
      family->next = prev_first;
   }
   release(family_list_lock);
   ```
6. This is the **clean up** step, in case of failure:
   1. If failure is after *Step 1*, free the memory allocated for family - `kfree(family)`.
   2. If failure is after *Step 2*, free up the page table, trampoline and heap - `thread_freepagetable(family->pagetable, family->sz);`. 


[**NOTE 1:** `thread_pagetable()` function creates and returns a pointer to the pagetable and also maps the trampoline page. It is derived from the older `proc_pagetable()` function.]

[**NOTE 2:** `thread_freepagetable()` function frees the page table and trampoline and heap pages and unmaps them. It is derived from the older `proc_freepagetable()` function.]

[**NOTE 3:** We don't initialize the rest of the shared family fields as `exec()` is expected to be called before the thread starts executing user code.]


```c
void free_family(struct thread_family_shared *family)
```
This function frees the family object. 

This is achieved by doing the following:
1. Acquire the family spin lock `spinlk`.
2. If `family->tcount > 0`, then release the lock and return.
3. Release the lock.
4. Acquire the family sleep lock `sleeplk`.
5. Free up the page table, trampoline and heap - `thread_freepagetable(family->pagetable, family->sz);`
6. Release the lock.
7. Remove the family's node from FCB by doing the following:
   1. Acquire FCB lock - `aquire(family_list_lock);`
   2. Loop over the TCB to find the family's node (`target`) and also the node just before it (`prev_target`):
      ```c
      struct thread *target = init_thread->family->next;
      struct thread *prev_target = init_thread->family;
      
      while (target != NULL){
         if (target->fid == family.fid) break;
         target = target->next;
         prev_target = prev_target->next;
      }

      if (target == NULL) {
         panic("Family node not found in FCB");
      }
      ```
   3. Remove the thread's node from TCB:
      ```c
      prev_target->next = target->next;
      ``` 
   4. Release the lock.
8. Free up the family memory - `kfree(family)`.



### 10. Creating a new sibling thread

```c
int clone(void (*fn)(void *), void *arg)
```
It creates a new thread and runs the function `fn` with arguments `arg`.  

To achieve this, it does the following:
1. Access calling thread's family - `struct thread_family_shared *family = mythread()->family;`
2. Check if cloning is allowed:
   1. Acquire spink lock `spinlk` on family
   2. If `family->no_clone == 1`, release lock and return -1
   3. Release lock
3. Create a thread object - `struct thread *td = NULL`.
4. Call `alloc_thread(family, &td)` to create a new thread. If failed, return -1.
5. Setup trapframe of the thread - 
   1. `td->trapframe->epc = fn`
   2. `td->trapframe->a0 = arg`
6. Set thread state as runnable:
   1. Acquire spink lock `lock` on thread
   2. Set state - `td->state = RUNNABLE`
   3. Release lock 
7. return `td->tid`.


### 11. Exit a thread from user space 

```c
void exit_thread(int status)
```
It is called by a thread when it wants to delete itself and is passed in a status argument which is the output of the thread execution.

To achieve this, it does the following:
1. Access calling thread - `struct thread *td = mythread()`
2. Access calling thread's family - `struct thread_family_shared *family = td->family;`
3. Acquire global spin lock `wait_lock`.
4. Decrement threads count:
   1. Acquire spin lock `spinlk` on family.
   2. Decrement threads counter - `family->tcount -= 1`
   3. Save current thread count - `int thread_count = family->tcount`
   4. Release lock
5. If `thread_count == 0`, then do the following or else go to *step 6*:
   1. Acquire sleep lock `sleeplk` on the family
   2. Clean up all externel resources of the family i.e. the open files, the current working directory inode, the VMAs.
   3. Release the `sleeplk` lock.
   4. Acquire global spin lock `family_list_lock`.
   5. Call `reparent()` to change the parent family of all of its children families to `init_thread->family`.
   6. Release the `family_list_lock` lock.
   7. Wake up a thread in the parent family, sleeping on the parent family's channel - `wakeup(family->parent_family)`.
6. Else, then we wake up a sibling thread sleeping on the family's channel - `wakeup(family)`.
7. Updating the dying state of the thread:
   1. Acquire spin lock `lock` on the thread
   2. Save the exit status - `td->xstate = status;`
   3. Change the state - `td->state = ZOMBIE;`
   [**NOTE :** The thread's spin lock is acquired, but not explicitly released here, as it should be held when calling `sched()` and released by the scheduler after context switch. ]
8. Release `wait_lock`.
9.  Jump into the scheduler, never to return to this thread - `sched()`.



### 12. Exit the entire thread family from user space

```c
void exit(int status)
```

It is called by a thread to exit its entire family.

It achieves this by doing the following:
1. Access calling thread - `struct thread *td = mythread()`
2. Access calling thread's family - `struct thread_family_shared *family = td->family;`
3. Turn off cloning:
   1. Acquire the spin lock no the family
   2. Turn off cloning - `family->no_clone = 1`
   3. Store siblings count - `int siblings = family->tcount - 1`
   4. Release lock
4. Acquire the global spin lock on the TCB (`thread_list_lock`)
5. Loop through the TCB and for every sibling child excluding itself, call `kill_thread(tid)`.
   ```c
   for (struct thread th in TCB){
      if (th->family == family && th != td){
         kill_thread(th->tid);
      }
   }
   ```
6. Release the lock
7. Wait for all sibling threads to exit:
   ```c
   for (int i = 0; i < siblings; i++) {
    join(NULL);
   }
   ```
8. Exit itself - `exit_thread(status)`


### 13. Copying thread state

```c
int copy_state_thread(struct thread *td)
```
It copies the calling thread's virtual memory, family state and thread state into the thread `td`. Will be used in the modified `fork()`.

It achieves this by doing the following:
1. Create instance of calling thread - `struct thread *calltd = mythread()`
2. Create instance of calling thread's family - `struct thread_family_shared *callfamily = calltd->family`
3. Create instance of `td`'s family - `struct thread_family_shared *family = td->family`.
4. Acquire locks on both families, ordered by address to avoid deadlocks
   ```c
   if (callfamily < family){
      acquire(callfamily->sleeplk);
      acquire(family->sleeplk);
   }else{
      acquire(family->sleeplk);
      acquire(callfamily->sleeplk);
   }
   ```
5. Copy the calling thread's ELF + heap into `td`'s virtual memory: `uvm_copy(callfamily->pagetable, family->pagetable, 0, callfamily->sz)`. On failure, return -1.
6. Get the user stack of calling thread - `uint64 ustack_base = ustack_top(callthread->slot_index) - PGSIZE`
7. Copy the calling thread's user stack into `td`'s virtual memory - `uvm_copy(callfamily->pagetable, family->pagetable, ustack_base, PGSIZE)`. On failure, return -1.
8. Increment reference counts on open file descriptors:
   ```c
   for(i = 0; i < NOFILE; i++)
    if(callfamily->ofile[i])
      family->ofile[i] = filedup(callfamily->ofile[i]);
   family->cwd = idup(callfamily->cwd);
   ```
9.  Copy heap size - `family->sz = callfamily->sz`
10. Copy heap reserve - `family->heap_reserve = callfamily->heap_reserve`
11. Set the slot tracking array for `td` - `family->slot_tracking[calltd->slot_index] = 1`
12. Copy calling thread's VMA's list - `memmove(family->vmas, callfamily->vmas, sizeof(callfamily->vmas));`
13. Increment reference count of VMA's file inodes:
   ```c
   for (int i=0; i<NVMA; i++){
      struct vma v = family->vmas[i];
      if (v.inode != 0){
         idup(v.inode);
      }
   }
   ```
14. Release locks on both families, ordered by address to avoid deadlocks
   ```c
   if (callfamily < family){
      release(callfamily->sleeplk);
      release(family->sleeplk);
   }else{
      release(family->sleeplk);
      release(callfamily->sleeplk);
   }
   ```
15. Copy the calling thread's trapframe into `td`'s trapframe - `*(td->trapframe) = *(calltd->trapframe)`
16. Copy calling thread name - `safestrcpy(td->name, calltd->name, sizeof(calltd->name));`
17. Copy calling thread's slot index - `td->slot_index = calltd->slot_index`
18. Return 0.


### 14. Derived Mechanisms
This subsection is dedicated to mechanisms which are derived from existing designs in vanilla xv6.

#### 1. Modified `int wait(uint64 addr)`
This allows a thread to wait for all the threads of a child family to exit.

Here, instead of looping over the processes to find a child zombie process, it will loop over families to find a dead child family i.e. `tcount == 0`.
If it is found, it then loops over the TCB to find all the zombie threads and for each such thread `zb_td`, it calls `free_thread(zb_td)`.
At last it then calls `free_family(dead_family)` to free the dead family, stores exit status in passed address and then return its FID.

If it doesn't find such a family, it sleeps on its own family as the channel, so that it is awoken when a child exits and it is the last child in its family. After it is awoken, it repeats the same process again.

#### 2. Derived `int join(uint64 addr)`
It allows a thread to wait for a sibling thread i.e. a thread belonging to the same family, to exit.

Its structure is very similar to the new `wait()`.
It loops over the TCB, to find a zombie sibling thread (`zb_td`).  
If it is found, it frees this thread by calling `free_thread(zb_td)`, stores exit status in passed address and then returns its TID 

If it doesn't find such a thread, it sleeps on its own family as the channel, so that it is awoken when a sibling thread exits. After it is awoken, it repeats the same process again.

#### 3. Derived `kill_thread(tid)`
It sets up the thread with the passed in TID to be killed.

To achieve this, it loops over all the threads in the TCB, and when it finds one with the matching TID, it sets `killed = 1` and then returns 0.

If no such thread is found, it returns -1.

#### 4. Modified `kill(fid)`
It sets up all the threads of the family with the passed in FID to be killed.

To achieve this, it first loops over the families and to find one with the matching FID. Once if finds this family, it calls `kill_thread()` on each of its threads and then returns 0.

If it doesn't such a family, it returns -1.

#### 5. Modified `exec()`
It overrides the calling thread's current running program with a new one.

To achieve this, it first turns off cloning and then kills all of its sibling threads and waits for them to exit using `join()`.

The rest of the execution is similar to the original, except that rather than setting up the fields on a process, we are doing it on the thread and its family. 

To be more precise, we override the family's page table (`pagetable`), heap size (`sz`), VMAs list (`vmas[]`). In case of the thread itself, we override the program counter (`trapframe->epc`) and user stack pointer (`trapframe->sp`) in trapframe and the kernel stack pointer in the context (`context->sp`)

In case of the user stack pointer, we place it back at the base of the already allocated user stack (in the thread's slot).

In case of the heap size, we place it at just after the loaded Program Code pages.

Also, for the `heap_reserve`, we recalculate it based on the new heap base.

The calling thread's slot is reused for the new program's slot, so no new slot allocation is needed.

#### 6. Modified `fork()`
It creates a new family with a new thread with the same execution state as the parent family and calling thread.

It makes use of a few functions that we have already defined to make this easier.
It creates a new family - `struct thread_family_shared *family = alloc_family()`.
It then creates a new thread - `struct thread *td = alloc_thread(family)`.
Then it uses `copy_state_thread(td)` which copies the calling thread's virtual memory, family state and thread state into the new thread `td`.

Have it return 0 in the new thread - `td->trapframe->a0 = 0`

Set child state to `RUNNABLE`.

Lastly, return the new family's FID to parent and 0 to child.

#### 7. Modified `uvm_copy()`
Right now, `uvm_copy()` only copies the memory starting from 0 to `sz`. We extend it so that it accepts a starting virtual address, and copies memory from there till it covers `sz`.

```c
int uvmcopy(pagetable_t old, pagetable_t new, uint64 start, uint64 sz)
```


#### 8. Modified Scheduler
The scheduler algorithm decides which one of the `RUNNABLE` threads gets to execute next on the CPU. 

Now instead of looping over the PCB, the scheduler loops over the TCB with the acquired global thread list lock to find the next thread to execute and acquires the per-thread lock to change the state of the thread. We then release the global lock before `swtch()`. The per-thread lock is held across `swtch()` and released by the newly scheduled thread after the context switch — consistent with vanilla xv6.

### 9. Modified `userinit()`
It sets up the first thread (`init_thread`) and its family (`init_family`).

Global variables `init_thread` and `init_family` are originally `NULL` initialized.

`userinit()` calls `alloc_family()` to allocate the first family and assigns it to `init_family`.
It then calls `alloc_thread(init_family)` to allocate the first thread and assigns it to `init_thread`.

It sets up the current working directory and marks the thread as `RUNNABLE`



## Alternatives Considered

### 1. Spin lock vs sleep lock for shared family fields
We considered using spinlocks for all fields in thread_family_shared for simplicity. However, as detailed in the [Thread Family Shared Data Structure](#2-new-thread-family-shared-data-structure) section, operations on pagetable, ofile, cwd and vmas can involve disk I/O which requires sleeping. Holding a spinlock while sleeping causes deadlock since interrupts are disabled. Hence sleep locks were chosen for these fields.

### 2. User-managed stacks in `clone()`
Linux's definition for `clone()` looks something like this:

```c
int clone(
    int (*fn)(void *),
    void *child_stack,
    int flags,
    void *arg
);
```
As you can see, the most important distiction here, is the `child_stack` pointer as 2nd input. Linux expects that the user will pass in a pointer to the top of a user allocated memory region (usually fromt the heap) which is incidently also the initial value of the stack pointer, by doing something like this:

```c
char *stack = malloc(STACK_SIZE);

clone(
    worker,
    stack + STACK_SIZE,  // stack grows downward
    CLONE_VM | CLONE_FILES | CLONE_FS,
    NULL
);
```
We have explicitly rejected this design and have the kernel allocate a fixed 1 page user stack for the new thread. We have done this to maintain simplicity of the interface, so that the user can just call the `clone()` function and not worry about allocating and deallocating the user stack while maintaining a guard page for catching stack overflows.


### 3. Family-level vs thread-level parenting
In vanilla xv6, processes have a parent-child relationship, in which a process will have one and only one parent (except for `initproc` which is the first process and thus has no parent, showed by having its `initproc->parent = NULL`) and can have many children.

When extending this idea to threads, it might seem natural for the threads in a family to also share a similar parent-child relationship, wherein the root thread i.e. the first thread created in the family has parent as `NULL` and other threads have threads in the same family as their parents. This idea seems perfectly complementary to the *family* notation that we were going with.

The problem with this arises when we talk about reparenting. Reparenting is the mechanism in which, when a thread/process is exiting, it changes the parent of all of its children to another thread/process, so that its children don't remain orphaned. In vanilla xv6, we reparented the children of an exiting process to `initproc` since we know that it will never exit.

But, when the root thread of a family exits, we can't perform reparenting easily. Who should be the parent of the root thread's children ?
- You can't make their parent `NULL` since that will create multiple root threads. 
- You can't assign their parent to be a thread other than the root thread's children, as any other thread, will be a decendent of a root thread's child, thereby creating a cyclic tree, which is a violation.
- You can't assign their parent to be a thread of another family since they will have completely different shared state.

Due to these reasons, we rejected the idea of a parent-child relationship between the family threads. All threads in a family have the same hierarchy.


On the other hand, this problem doesn't come up when we have parent-child relationships between **families**. When a family exits, it assigns `init_thread`'s (it is first thread ever created by the system and is the one and only thread in its family) family as the new parent to its children. We can confidently assign these children families to `init_thread->family` as we know that `init_thread` will never exit.



### 4. `usertrap()` level page table locking
In vanilla xv6, there was no need for any concurrency protection to be given to the page table, as each process's page table will only be accessed by the process itself.

But now after introducing multi-threading, wherein a family of threads shares a page table, it is very important to protect the page table with locks (sleep locks in this case) on every page table access.

But page tables are accessed many times throughout the code base, and going and adding locks to each access can be tedious and missing even a single one can lead to silent bugs and race conditions. Thus we were thinking whether there is a single place that we can add these locks to protect the page table access completely.

And the most obvious place came as `usertrap()` since all page tables accesses pass through it. But for this, we will need to acquire the lock at the start of `usertrap()` and release it before it returns. The biggest issue with this, is that threads from the same family, but on different cores, will never be able to enter `usertrap()` at the same time. This completely serializes `usertrap()`, defeating the purpose of multi-threading.


Hence, at the end, we decided to protect each occurrence of page table access with locks individually. While this offers perfect protection, it still reduces concurrency since more than one thread can't access the page table simultaneously even for reads.



### 5. Identity-mapped vs Explicitly-mapped Kernel Stacks

When allocating kernel stacks, we could have used the physical address of the kernel stack as its virtual address itself (since it is identity mapped) as we know that it will be unique. There is no need for any mappings, like there is for user stack, since user stacks exists in Userspace Virtual Memory, which is defined by the user page table wherein a physical address of memory can be mapped to any virtual address. This is necessary as the physical address of the memory allocated for user stack, might fall into a different region than what is defined by the [User Space Memory Layout](#3-new-memory-layout).

But the problem with this decision of using identity mapped VA for kernel stacks, is that we can't guarantee that the page of memory below it will always remain unallocated to act as a guard page. It can readily be picked up by any `kalloc()` access. The only way to guarantee that the page below is unoccupied is to allocate it and mark it as read-only so writing to it causes a page fault. But this wastes a physical page of memory per kernel stack, just to act as a guard page.

A better alternative is to allocate a page of memory and map it to a kernel space virtual address, and leave the page after that as unmapped so that it can act as a guard page to catch stack overflows through page faults. Hence why we take this approach, and thus we need the global `kstack_tracking` array to track the kernel allocations according to the new [Kernel Space Memory Layout](#kernel-virtual-address-space)

## Future Work

1. A **Read-write Lock** for the page table in the shared family structure would increase parallelism for page table access. Currently, all page table accesses are serialized through a single sleep lock, even read-only operations that could safely run concurrently. A read-write lock would allow multiple threads to read from the page table simultaneously, while still serializing writes."
2. Using a **Concurrent Linked List** for the TCB and FCB will increase concurrency as it would allow multiple threads to modify the structure of the TCB and FCB in parallel.
3. A **Production-grade Virtual Memory Allocator**, similar to Linux's mmap(), would address several limitations of our current fixed formula-based memory layout. Currently, each thread stack is fixed at one page and cannot grow, and stacks and heap compete for the same free virtual address space, limiting both the number of threads and heap growth. A flexible allocator would allow dynamic stack growth, unconstrained heap growth, and flexible placement of memory regions — effectively removing the hard dependency on `HEAP_RESERVE` as a static boundary.