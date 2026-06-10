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
- `void thread_exit(void);`

`clone()` creates a new child thread from the currently running thread. The caller provides a function pointer indicating where the new thread should begin execution, along with the argument to pass to that function. `thread_exit()` terminates the calling thread.

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

### 1.  New Thread Data Structure 

```c
struct thread {
  struct spinlock lock;

  enum threadstate state;      
  void *chan;                  
  int killed;                  
  int xstate;                  
  int tid;                     

  struct thread *parent;        

  int kstack_index;  
  int slot_index;             
  struct trapframe *trapframe; 
  struct context context;
  
  struct thread_family_shared *family;

  char name[16];              
};

```
The new `struct thread` contains only the per-thread state required for scheduling, trap handling, and lifecycle management. Unlike the original `struct proc`, it does not contain resources that are shared across all threads in a thread family, such as the address space, open file table, current working directory etc. These shared fields have been moved to a separate shared data structure, to which the `family` field is a pointer to. This is described in more detail in the next section.

`slot_index` is the index that this thread's slot has occupied in the `slot_tracking` array (covered in detail in the subsection 2). It has an uninitialized value of `-1`.

`kstack_index` is the index that this thread's kernel stack has occupied in the global `kstack_tracking` array (covered in detail in the subsection 6). It has an uninitialized value of `-1`.

The structure is protected by a **spin lock** rather than a sleep lock. The scheduler frequently accesses and updates thread state while selecting runnable threads. If contention on the lock caused the scheduler to sleep, scheduling itself could be blocked waiting for access to thread state, creating the possibility of deadlock. Since these operations involve only short in-memory updates, a spin lock is a more appropriate choice.


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

  int tcount;
  int no_clone;
  int slot_tracking[NFAMILY_THREADS];
  uint64 heap_reserve;
};

```
The new `struct thread_family_shared` contains the fields common to a thread family such as the address space via the page table, the heap size (i.e. the top of the heap), the list of open files, the inode pointer to the current working directory and the list of VMAs, needed to lazy load the ELF segments of the program.

**NOTE:** The sleep lock should be held wherever any kind of page table access needs to be made. So the existing page table access need to be modified to hold the lock.

Each thread in the same family have their `family` pointer pointing to the same `struct thread_family_shared` object.

The `slot_tracking` array is a boolean array. If `slot_tracking[slot_index] == 1` then the thread slot at index `slot_index` is occupied, else it is unoccupied. This array is useful when assigning a **thread slot** memory region to a new thread. For a given thread's slot index, we can derive formulas for calculating the address of the thread slot's base, the top of the user stack and the base of the trapframe. These formulas are mentioned in the **Virtual Memory Layout** subsection coming up next.

`NFAMILY_THREADS` is the absolute maximum number of threads that you can create in a single thread family. Its limited by the size of the **User Virtual Address Space** as it can accommodate only a limited number of thread slots.

`heap_reserve` is the boundary beyond which thread slots can't be allocated. This will be explained in more detail in the New Memory Layout subsection

The `no_clone` boolean flag, basically doesn't allow for a thread to be cloned (i.e. another thread to be created) if it is True. We need to turn off cloning especillay when we call `exec()` and kill off all the other family threads, and don't want any new threads created and also in `exit()` when we want to kill all the threads in the family and hence don't want any new threads to be created.

The `tcount` field keeps count of the number of threads alive in this family.

We use a **sleep lock** (`sleeplk`) to protect the shared thread-family state of `pagetable`, `ofile`, `cwd` and `vmas`. Operations on shared resources such as the page table, open file table, current working directory, and VMAs may involve acquiring inode locks, waiting for disk I/O or other actions that can cause the calling thread to sleep. A spin lock is unsuitable here because xv6 disables interrupts while a spinlock is held. If a thread holding a spinlock goes to sleep waiting for an event that depends on an interrupt (such as disk I/O completion), the interrupt cannot be delivered, resulting in a deadlock. A sleep lock avoids this issue by allowing the thread to block and yield the CPU while waiting for the resource to become available.

We use a **spin lock** (`spinlk`) to protect the thread count (`tcount`) and user stack tracking array (`ustack_tracking`) as their operations have short critical sections and aren't dependent on interrupts.


### 3. New Linked List based Thread Control Block (TCB)

```c
struct tnode {
  struct thread thread;
  struct tnode *next;
};

struct tnode *thead;

struct spinlock tnodes_lock;

```

Earlier we had a fixed array as the PCB, but now we move to a **Linked List** for the `TCB`. We choose this approach as this allows us to dynamically allocate threads without any strict upper bound imposed by the list. The downside is that to the traversing to a specific thread takes `O(n)` time (vs `O(1)` in earlier approach via array index). But this is fine as the scheduler loops over all the threads anyway.

The `thread` field is the actual thread represented by this node and `next` is a pointer to the next node in the list. 

The `thead` is the head of the Linked List. For the first thread created, its `tnode` will be attached as the next to `thead`.

`tnodes_lock` is a global lock for this list's structure. Hence, when we loop over the list or add or remove nodes, we need to acquire this lock. On the other hand, the per-thread lock in `thread` is used when modifying the thread itself. To avoid deadlocks, you acquire the `tnodes_lock` first and then find the thread, then acquire a lock on the thread, release `tnodes_lock`, make the changes you want on the thread and then release the lock on the thread.

### 4. New Memory Layout 

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



### 5. Per-thread Slot handling with Per-thread Slot Tracking array 
The  `clone()` function needs to assign a user stack succesfully to create a new thread. It will call the function whos definition is given below. `family` is the pointer to the family shared struct. `base` is the pointer to the ustack's base address, which the function will assign value to if the allocation is succesful. The function will return `0` on success and `-1` on failure.
```c
int alloc_slot(struct thread_family_shared *family, struct thread *td)
```
The `alloc_slot()` function will follow these steps:

1. Acquire the `spinlk` lock on the `thread_family_shared`. Find an empty stack slot by looping over `slot_tracking` and finding first index where `slot_tracking[index] == 0`. Lets call this index `slotIdx`. If such an index is found, mark `slot_tracking[slotIdx] = 1`. Release the lock. If such an index is not found then go to **Step 7**
2. Mark slot index - `td->slot_index = slotIdx;`
3. Find the `ustack_top` and `trapframe_base` addresses using the above given formula.
4. Make sure the ustack & guard-page are not overlapping with the heap and they are above the `heap_reserve`.  
   ```c
    ustack_top - 2*PGSIZE > thread_family_shared->sz && ustack_top - 2*PGSIZE > thread_family_shared->heap_reserve
   ```
   If this condition is not true, go to **Step 7**
5. Allocate the user stack by doing the following:
   1.  Get a page of memory by calling `kalloc()`. If it fails, then go to **Step 7**.
   2.  Acquire the sleep lock on `thread_family_shared`
   3.  Map the page from `ustack_top - PGSIZE` address using `mappages()`. If `mappages()` fails, release the lock and go to **Step 7**.
   4.  Keep the guard page unmapped, so that it can catch any stack overflows. 
   5.  Return `0` for success.
6. Allocate the trapframe by doing the following:
   1.  Get a page of memory by calling `kalloc()`. If it fails, then go to **Step 7**.
   2.  Acquire the sleep lock on `thread_family_shared`
   3.  Map the page from `trapframe_base` address using `mappages()`. If `mappages()` fails, release the lock and go to **Step 7**.
   4.  Return `0` for success.
7. This is the **clean up** step, in case of failure. 
   1. Free the stack page, if allocated. 
   2. Free the trapframe page, if allocated.
   3. If user stack `mappages()` succeeded before failure, then we need to acquire the sleep lock on `thread_family_shared` and unmap the `ustack` page from the page table using `uvmunmap()` and then release the lock
   4. If traprame `mappages()` succeeded before failure, then we need to acquire the sleep lock on `thread_family_shared` and unmap the `trapframe` page from the page table using `uvmunmap()` and then release the lock
   5. If the `slot_tracking[slotIdx]` is marked, then we acquire the spin lock on `thread_family_shared`, and then we unmark it as `slot_tracking[slotIdx] = 0`.  Now, we release the lock.
   6. If `td->slot_index` is marked, then we unmark it - `td->slot_index = -1`
   7. Return `-1` for failure.

Also, the slot needs to be freed on thread exit.
```c
void free_slot(struct thread_family_shared *family, struct thread *td);
```
For this we follow these steps:
1. Acquire the sleep lock on `thread_family_shared` 
2. Unmap the stack page from page table, and free the stack page, using `uvmunmap()`.
3. Unmap the trapframe page from page table, and free the stack page, using `uvmunmap()`.
4. Release the lock.
5. Acquire the spin lock on the thread's `thread_family_shared` struct.
6. Mark the stack memory region as free as such - `ustack_tracking[td->slot_index] = 0;`.
7. Release the lock
8. Unmark the slot index - `td->slot_index = -1;`.


### 6. Per-thread Kernel Stack handling with Global Kernel Stack Tracking array 
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
1. Acquire the global `kstack_tracking_lock` lock. Find an empty stack slot by looping over `kstack_tracking` and finding first index where `kstack_tracking[index] == 0`. Lets call this index `kstackIdx`. If such an index is found, mark `kstack_tracking[kstackIdx] = 1`. Release the lock. If such an index is not found then go to **Step 6**
2. Find the `kstack_base` address using the above given formula and the `kstackIdx`.
3. Allocate the kernel stack by doing the following:
   1. Get a page of memory by calling `kalloc()`. If it fails, then go to **Step 6**. 
   2. Acquire the spin lock on `kpgtbl_lock` and map the page to the `kstack_base` address using `mappages()`. If `mappages()` fails, go to **Step 6**. Keep the guard page unmapped, so that it can catch any stack overflows. 
   3. Release the lock. 
4. Assign kernel stack index - `td->kstack_index = kstackIdx;`.
5.  Return `0` for success.
6. This is the **clean up** step, in case of failure. 
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


### 7. New allocthread() function
It is used to create a new thread. Its definition is given below.
```c
int allocthread(struct thread *parent);
```
Here `parent` is the pointer to the caller thread. It will be `NULL` if its the first thread of the family.

The `allocthread()` function the following steps:
1. Initialize a new thread object - `struct thread *child;`
2. if `parent == NULL` i.e. this is the first thread in the family, then we need to initialize the shared family struct by doing the following:
   1. Initialize empty shared family object - `struct thread_family_shared *family;`.
   2. Allocate page for trapframe =  
   3. Initialize empty user page table - `family->pagetable = thread_pagetable(child)`.
   4. Set thread count to 1 - `family->tcount = 1`.
   5. Zero initialze some family fields - 
      ```c
      family->ustack_tracking = {0};
      family->no_clone = 0;
      ```
      [**NOTE:** In case of `parent == NULL`, we don't initialize the rest of the shared family fields as `exec()` is expected to be called before the thread starts executing user code.]
3. Else if `parent != NULL`, do the following: 
   1. Use the shared struct from parent - `struct thread_family_shared *family = parent->family;`.
   2. Increment thread count - `parent->family.tcount += 1`;.
4. Allocate user stack by calling -  `alloc_ustack(family, child->ustack_base)`.
5. Allocate kernel stack by calling - `alloc_kstack(family, child->kstack)`.
6. Initialze the **return address** - `child->context.ra = forkret`.
7. Create new TCB node - 
   ```c
   struct tnode childnode {
      .thread = child,
      .next = NULL
   };
   ```
8. Acquire TCB lock, insert new node into first position in TCB List and then release the lock - 
   ```c
   aquire(tnodes_lock);
   struct tnode nxt = thead.next;
   thead.next = childnode;
   childnode.next = nxt;
   release(tnodes_lock);
   ```

New clone() user space function 

Modified exit() function

New thread_exit() user space function 

Modified exec() function 

Modified fork() function 

Modified Scheduler

