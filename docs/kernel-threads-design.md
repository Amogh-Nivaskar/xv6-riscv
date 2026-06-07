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
- int clone(void (*fn)(void *), void *arg);
- void thread_exit(void);

clone() creates a new sibling thread from the currently running thread. The caller provides a function pointer indicating where the new thread should begin execution, along with the argument to pass to that function. thread_exit() terminates the calling thread.

We also need to modify the behavior of some existing system calls:

- `fork()` should copy only the calling thread into the child process, and not any of its sibling threads.
- `exec()` should terminate all sibling threads before replacing the calling thread's full thread family with the new program image.

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
8. Create new syscall `clone()` to create a new sibling thread.
9. Create new syscall `thread_exit()` to exit the calling thread.
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

### Kernel Stacks
Each process has its own kernel stack. When a process enters kernel mode (for eg: during a syscall), it switches its SP (stack pointer) from the user stack (ustack) to the kernel stack (kstack).  
When the kernel boots, the kstacks for all the `NPROC` processes are initialized in the kernel page table. `procinit()` initializes the kstack pointers for all `NPROC` processes using `KSTACK(i)`, which is a fixed formula to compute the kstack address via the process index `i`. The actually kstack allocation for each process is done in  `proc_mapstacks()`, which allocates a page from free-list using `kalloc()` and maps it to kernel page table at the Virtual Address (VA) calculated using `KSTACK(i)`.

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

### New Thread Data Structure 

```c
struct thread {
  struct spinlock lock;

  enum threadstate state;      
  void *chan;                  
  int killed;                  
  int xstate;                  
  int tid;                     

  struct thread *parent;        

  uint64 kstack;  
  uint64 ustack_base;             
  struct trapframe *trapframe; 
  struct context context;
  
  struct thread_family_shared *family;

  char name[16];              
};

```
The new `struct thread` contains only the per-thread state required for scheduling, trap handling, and lifecycle management. Unlike the original `struct proc`, it does not contain resources that are shared across all threads in a thread family, such as the address space, open file table, current working directory etc. These shared fields have been moved to a separate shared data structure, to which the `family` field is a pointer to. This is described in more detail in the next section.

`ustack_base` is the address of the base of the thread's user stack, which we will need when freeing the user stack memory. In our notation, the base is where the **Stack Pointer** (`SP`) starts from, i.e. at the top and grows downwards. 

The structure is protected by a **spin lock** rather than a sleep lock. The scheduler frequently accesses and updates thread state while selecting runnable threads. If contention on the lock caused the scheduler to sleep, scheduling itself could be blocked waiting for access to thread state, creating the possibility of deadlock. Since these operations involve only short in-memory updates, a spin lock is a more appropriate choice.


### New Thread Family Shared Data Structure

```c
struct thread_family_shared {
  struct sleeplock sleeplk;
  struct spinlock spinlk;

  pagetable_t pagetable;  
  struct file *ofile[NOFILE];
  struct inode *cwd;
  struct vma vmas[NVMA];

  int tcount;
  int no_clone;
  int ustack_tracking[NTHREAD];
  uint64 heap_reserve;
};

```
The new `struct thread_family_shared` contains the fields common to a thread family such as the address space via the page table, the list of open files, the inode pointer to the current working directory and the list of VMAs, needed to lazy load the ELF segments of the program.

Each thread in the same family have their `family` pointer pointing to the same `struct thread_family_shared` object.

The `ustack_tracking` array is a boolean array. If `ustack_tracking[i] == 1` then the ustack region at index `i` is occupied, else it is unoccupied. This array is useful when assigning a `ustack` memory region to a new thread. The formula of calculating the base address of a ustack from its index is given below, and is derived from the new **Virtual Memory Layout** explained in detail in the next section.

`NTHREAD` is the absolute maximum number of threads that you can create in a single thread family. Its limited by the size of the **User Virtual Address Space** as it can accommodate only a limited number of user stacks.

`heap_reserve` is the boundary beyond which user stacks can't be allocated. This will be explained in more detail in the New Memory Layout subsection

```c
ustack_base(i) = MAXVA - (2 * PGSIZE) - (i * 2 * PGSIZE)
```

The `no_clone` boolean flag, basically doesn't allow for a thread to be cloned (i.e. another thread to be created) if it is True. We need to turn off cloning especillay when we call `exec()` and kill off all the sibling threads, and don't want any new threads created and also in `exit()` when we want to kill all the threads in the family and hence don't want any new threads to be created.

The `tcount` field keeps count of the number of threads alive in this family.

We use a **sleep lock** (`sleeplk`) to protect the shared thread-family state of `ofile`, `cwd` and `vmas`. Operations on shared resources such as the  open file table, current working directory, and VMAs may involve acquiring inode locks, waiting for disk I/O or other actions that can cause the calling thread to sleep. A spin lock is unsuitable here because xv6 disables interrupts while a spinlock is held. If a thread holding a spinlock goes to sleep waiting for an event that depends on an interrupt (such as disk I/O completion), the interrupt cannot be delivered, resulting in a deadlock. A sleep lock avoids this issue by allowing the thread to block and yield the CPU while waiting for the resource to become available.

We use a **spin lock** (`spinlk`) to protect the thread count (`tcount`) and user stack tracking array (`ustack_tracking`) as their operations have short critical sections and aren't dependent on interrupts.


### New Linked List based Thread Control Block (TCB)

```c
struct tnode {
  struct thread thread;
  struct tnode *next;
};

struct tnode *thead;

struct spinlock tnodeslk;

```

Earlier we had a fixed array as the PCB, but now we move to a **Linked List** for the `TCB`. We choose this approach as this allows us to dynamically allocate threads without any strict upper bound imposed by the list. The downside is that to the traversing to a specific thread takes `O(n)` time (vs `O(1)` in earlier approach via array index). But this is fine as the scheduler loops over all the threads anyway.

The `thread` field is the actual thread represented by this node and `next` is a pointer to the next node in the list. 

The `thead` is the head of the Linked List. For the first thread created, its `tnode` will be attached as the next to `thead`.

`tnodeslk` is a global lock for this list's structure. Hence, when we loop over the list or add or remove nodes, we need to acquire this lock. On the other hand, the per-thread lock in `thread` is used when modifying the thread itself. To avoid deadlocks, you acquire the `tnodeslk` first and then find the thread, then acquire a lock on the thread, release `tnodeslk`, make the changes you want on the thread and then release the lock on the thread.

### New Memory Layout 

```text
MAXVA
+-------------------------------+
|          Trampoline           |  1 page
+-------------------------------+
|           Trapframe           |  1 page
+-------------------------------+
|       User Stack #0 ↓         |  1 page
+-------------------------------+
|         Guard Page            |  1 page
+-------------------------------+
|       User Stack #1 ↓         |  1 page
+-------------------------------+
|         Guard Page            |  1 page
+-------------------------------+
|       User Stack #2 ↓         |  1 page
+-------------------------------+
|         Guard Page            |  1 page
+-------------------------------+  ← Additional stack & guard-page pairs
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

In the new memory layout, the thread stacks are allocated from the high end of the user address space and extend downward toward lower virtual addresses. Each thread is assigned a dedicated one-page user stack along with a one-page guard page. The guard page remains unmapped and serves as a protection mechanism against stack overflows. Since stack allocation proceeds downward through the free address space, newly created threads receive stack & guard-page pairs at progressively lower virtual addresses. Each user stack is fixed at one page in size and does not grow dynamically. However, the stack pointer is initialized at the highest address within the stack page and moves downward as stack frames are pushed during execution.

The heap starts with one page and grows upward via `sbrk()` while thread stack allocation progresses downward, both regions compete for the same free address space. A naïve allocation strategy would allow thread creation to consume all available free space, potentially preventing future heap growth. To avoid this situation, the design introduces a reservation boundary, `HEAP_RESERVE`, within the free address space. New thread stacks may only be allocated above this boundary, ensuring that a portion of the address space remains available for future heap expansion.

`HEAP_RESERVE` is not a hard limit on heap growth. The boundary only constrains stack allocation. If the heap requires additional pages and free space exists above the reservation boundary, it is permitted to grow beyond `HEAP_RESERVE`. In effect, the reservation acts as a one-way constraint: thread stacks may not cross below the boundary, but the heap may grow beyond it when necessary. The `HEAP_RESERVE` address is stored in the `struct thread_family_shared` in field `heap_reserve`. It is computed in `exec()` using the following formula - 
```c
heap_reserve = initial_sz + (HEAP_RESERVE_PAGES * PGSIZE)
```
`HEAP_RESERVE_PAGES` are the number of pages we want to reserve for the heap. For our implementation, we will go with `HEAP_RESERVE_PAGES = 4`. The `initial_sz` is the `sz` value in `exec()`, which is basically the upper boundary of ELF. 

This approach provides a simple and predictable balance between thread creation and dynamic memory allocation. By reserving address space for future heap growth, the design prevents excessive thread creation from starving the heap while still allowing unused reserved space to be utilized by the heap when required.



### Per-thread User Stack handling with Per-thread User Stack Tracking array 

Now we can also understand how the formula - `ustack_base(i) = MAXVA - (2 * PGSIZE) - (i * 2 * PGSIZE)` - mentioned above was derived. From `MAXVA` we subtract 2 pages for the `Trampoline` and the `Trapframe`. Then we subtract all the stack & guard-page allocations before index `i` and we arrive at the user stack base. The allocation of the user stack will be discussed in the below section.


Per thread Kernel Stack handling with Global Kernel Stack Tracking array 

New allocthread() function

New clone() user space function 

Modified exit() function

New thread_exit() user space function 

Modified exec() function 

Modified fork() function 

Modified Scheduler

