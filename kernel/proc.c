#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include <stddef.h>

struct cpu cpus[NCPU];

struct thread *init_thread;
struct thread_family_shared *init_family;

struct spinlock thread_list_lock;
struct spinlock family_list_lock;

int kstack_tracking[NTHREADS];
struct spinlock kstack_tracking_lock;

extern pagetable_t kernel_pagetable;

int nexttid = 1;
int nextfid = 1;
struct spinlock tid_lock;
struct spinlock fid_lock;

extern void forkret(void);

static void free_thread(struct thread *td);
static void free_family(struct thread_family_shared *f);

extern char trampoline[]; // trampoline.S

extern uint ticks;

int family_count = 0;

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// initialize the proc table.
void procinit(void)
{
  initlock(&tid_lock, "nexttid");
  initlock(&fid_lock, "nextfid");
  initlock(&wait_lock, "wait_lock");
  initlock(&kstack_tracking_lock, "kstack_tracking_lock");
  initlock(&thread_list_lock, "thread_list_lock");
  initlock(&family_list_lock, "family_list_lock");
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct thread *, or zero if none.
struct thread *
mythread(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct thread *td = c->thread;
  pop_off();
  return td;
}

struct thread_family_shared *
myfamily(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct thread_family_shared *fm = c->thread->family;
  pop_off();
  return fm;
}

int alloctid()
{
  int tid;

  acquire(&tid_lock);
  tid = nexttid;
  nexttid = nexttid + 1;
  release(&tid_lock);

  return tid;
}

int allocfid()
{
  int fid;

  acquire(&fid_lock);
  fid = nextfid;
  nextfid = nextfid + 1;
  release(&fid_lock);

  return fid;
}

int alloc_kstack(struct thread *t)
{
  int kstackIdx = -1;
  acquire(&kstack_tracking_lock);
  for (int i = 0; i < NTHREADS; i++)
  {
    if (kstack_tracking[i] == 0)
    {
      kstackIdx = i;
      break;
    }
  }

  if (kstackIdx == -1)
  {
    release(&kstack_tracking_lock);
    return -1;
  }

  kstack_tracking[kstackIdx] = 1;

  t->kstack_index = kstackIdx;

  uint64 kstack_base = KSTACK_BASE(t->kstack_index);
  t->context.sp = kstack_base + PGSIZE;

  char *pa = kalloc();

  if (pa == 0)
  {
    kstack_tracking[kstackIdx] = 0;
    t->kstack_index = -1;
    t->context.sp = 0;
    release(&kstack_tracking_lock);
    return -1;
  }
  release(&kstack_tracking_lock);

  kvmmap(kernel_pagetable, kstack_base, (uint64)pa, PGSIZE, PTE_R | PTE_W);

  return 0;
}

void free_kstack(struct thread *t)
{
  uint64 kstack_base = KSTACK_BASE(t->kstack_index);

  kvmunmap(kernel_pagetable, kstack_base, 1, 1);

  acquire(&kstack_tracking_lock);
  kstack_tracking[t->kstack_index] = 0;
  release(&kstack_tracking_lock);

  t->kstack_index = -1;
  t->context.sp = 0;
}

int alloc_slot(struct thread *t, pagetable_t pagetable)
{
  struct thread_family_shared *family = t->family;

  int old_slotIdx = t->slot_index;
  struct trapframe *old_trapframe = t->trapframe;

  int new_slotIdx = -1;

  acquire(&family->spinlk);
  for (int i = 0; i < NFAMILY_THREADS; i++)
  {
    if (family->slot_tracking[i] == 0)
    {
      new_slotIdx = i;
      break;
    }
  }

  if (new_slotIdx == -1)
  {
    release(&family->spinlk);
    goto err_no_empty_slot_index;
  }

  family->slot_tracking[new_slotIdx] = 1;

  t->slot_index = new_slotIdx;

  uint64 slot_base = SLOT_BASE(t->slot_index);
  uint64 ustack_top = USTACK_TOP(t->slot_index);
  uint64 trapframe_base = TRAPFRAME_BASE(t->slot_index);

  if (slot_base <= family->sz || slot_base <= family->heap_reserve)
  {
    release(&family->spinlk);
    goto err_slot_index_filled;
  }

  release(&family->spinlk);

  char *trapframe_pa = kalloc();

  if (trapframe_pa == 0)
  {
    goto err_slot_index_filled;
  }

  acquiresleep(&family->sleeplk);
  if (mappages(pagetable, trapframe_base, PGSIZE,
               (uint64)trapframe_pa, PTE_R | PTE_W) < 0)
  {
    releasesleep(&family->sleeplk);
    goto err_trapframe_allocated;
  }

  releasesleep(&family->sleeplk);

  t->trapframe = (struct trapframe *)trapframe_pa;

  char *ustack_pa = kalloc();

  if (ustack_pa == 0)
  {
    goto err_trapframe_mapped;
  }

  acquiresleep(&family->sleeplk);
  if (mappages(pagetable, ustack_top - PGSIZE, PGSIZE,
               (uint64)ustack_pa, PTE_R | PTE_W | PTE_U) < 0)
  {
    releasesleep(&family->sleeplk);
    goto err_ustack_allocated;
  }
  releasesleep(&family->sleeplk);

  t->trapframe->sp = ustack_top;

  return 0;

err_ustack_allocated:
  kfree(ustack_pa);

err_trapframe_mapped:
  acquiresleep(&family->sleeplk);
  uvmunmap(pagetable, trapframe_base, 1, 0);
  releasesleep(&family->sleeplk);

err_trapframe_allocated:
  kfree(trapframe_pa);

err_slot_index_filled:
  acquire(&family->spinlk);
  family->slot_tracking[t->slot_index] = 0;
  release(&family->spinlk);
  t->slot_index = old_slotIdx;
  t->trapframe = old_trapframe;

err_no_empty_slot_index:
  return -1;
}

void free_slot(struct thread *t)
{
  acquiresleep(&t->family->sleeplk);
  unmap_slot(t->slot_index, t->family->pagetable, 1);
  t->family->slot_tracking[t->slot_index] = 0;
  releasesleep(&t->family->sleeplk);

  acquire(&t->lock);
  t->slot_index = -1;
  t->trapframe = NULL;
  release(&t->lock);

  return;
}

void unmap_slot(int slot_index, pagetable_t pagetable, int do_free)
{
  uint64 ustack_top = USTACK_TOP(slot_index);
  uint64 trapframe_base = TRAPFRAME_BASE(slot_index);

  uvmunmap(pagetable, ustack_top - PGSIZE, 1, do_free);

  uvmunmap(pagetable, trapframe_base, 1, do_free);
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
struct thread *alloc_thread(struct thread_family_shared *f)
{
  struct thread *t;

  if ((t = (struct thread *)kalloc()) == 0)
  {
    return NULL;
  }
  memset(t, 0, sizeof(*t));

  initlock(&t->lock, "thread");

  t->tid = alloctid();
  t->state = USED;
  t->kstack_index = -1;
  t->slot_index = -1;
  t->family = f;

  // Allocate a slot.
  if (alloc_slot(t, t->family->pagetable) == -1)
  {
    kfree(t);
    return NULL;
  }

  if (alloc_kstack(t) == -1)
  {
    free_slot(t);
    kfree(t);
    return NULL;
  }

  t->next = NULL;

  acquire(&thread_list_lock);
  if (init_thread == NULL)
  {
    init_thread = t;
  }
  else
  {
    t->next = init_thread->next;
    init_thread->next = t;
  }
  release(&thread_list_lock);

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&t->context, 0, sizeof(t->context));
  t->context.ra = (uint64)forkret;
  t->context.sp = KSTACK_BASE(t->kstack_index) + PGSIZE;
  t->cpu_time = 0;
  t->priority = 0;
  t->first_runnable_tick = -1;
  t->last_runnable_tick = -1;
  t->first_run_tick = -1;
  t->exit_tick = -1;
  t->runs_count = 0;
  t->total_wait_time = 0;
  return t;
}

struct thread_family_shared *alloc_family()
{
  struct thread_family_shared *f;

  if ((f = (struct thread_family_shared *)kalloc()) == 0)
  {
    return NULL;
  }
  memset(f, 0, sizeof(*f));

  initlock(&f->spinlk, "family");
  initsleeplock(&f->sleeplk, "family");

  f->tcount = 1;

  // An empty user page table.
  f->pagetable = family_pagetable();
  if (f->pagetable == 0)
  {
    kfree(f);
    return NULL;
  }

  f->next = NULL;

  acquire(&family_list_lock);

  if (family_count >= NFAMILIES)
  {
    family_freepagetable(f->pagetable, f->sz);
    kfree(f);
    release(&family_list_lock);
    return NULL;
  }
  else
  {
    family_count++;
  }

  f->fid = allocfid();

  if (init_family == NULL)
  {
    init_family = f;
  }
  else
  {
    f->next = init_family->next;
    init_family->next = f;
  }
  release(&family_list_lock);
  return f;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
free_thread(struct thread *t)
{
  if (t == init_thread)
    panic("free_thread: attempting to free init_thread");

  if (t->slot_index != -1)
    free_slot(t);

  if (t->kstack_index != -1)
    free_kstack(t);

  acquire(&thread_list_lock);
  struct thread *target = init_thread->next;
  struct thread *prev_target = init_thread;

  while (target != NULL)
  {
    if (target->tid == t->tid)
      break;
    target = target->next;
    prev_target = prev_target->next;
  }

  if (target == NULL)
  {
    panic("Thread node not found in TCB");
  }
  prev_target->next = target->next;
  release(&thread_list_lock);

  kfree(target);
}

static void
free_family(struct thread_family_shared *f)
{
  if (f == init_family)
    panic("free_family: attempting to free init_family");

  if (f->pagetable)
    family_freepagetable(f->pagetable, f->sz);

  acquiresleep(&f->sleeplk);
  for (int i = 0; i < NVMA; i++)
  {
    struct vma v = f->vmas[i];
    if (v.inode != 0)
    {
      iput(v.inode);
    }
  }
  memset(f->vmas, 0, sizeof(f->vmas));
  releasesleep(&f->sleeplk);

  acquire(&family_list_lock);
  struct thread_family_shared *target = init_family->next;
  struct thread_family_shared *prev_target = init_family;

  while (target != NULL)
  {
    if (target->fid == f->fid)
      break;
    target = target->next;
    prev_target = prev_target->next;
  }

  if (target == NULL)
  {
    panic("Family node not found in FCB");
  }
  prev_target->next = target->next;

  family_count--;
  release(&family_list_lock);

  kfree(target);
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
family_pagetable()
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
  {
    return 0;
  }

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void family_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void userinit(void)
{

  if ((init_family = alloc_family()) == NULL)
  {
    panic("userinit: no page for init_family");
  }

  if ((init_thread = alloc_thread(init_family)) == NULL)
  {
    panic("userinit: no page for init_thread");
  }

  acquire(&init_family->spinlk);
  init_family->cwd = namei("/");
  release(&init_family->spinlk);

  acquire(&init_thread->lock);
  init_thread->family = init_family;
  init_thread->state = RUNNABLE;
  init_thread->first_runnable_tick = ticks;
  init_thread->last_runnable_tick = ticks;
  release(&init_thread->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint64 sz;
  struct thread *t = mythread();
  struct thread_family_shared *f = t->family;

  int last_slotIdx = -1;

  acquire(&f->spinlk);
  for (int i = 0; i < NFAMILY_THREADS; i++)
  {
    if (f->slot_tracking[i] == 1 && i > last_slotIdx)
    {
      last_slotIdx = i;
    }
  }
  release(&f->spinlk);

  uint64 last_slot_base = SLOT_BASE(last_slotIdx);

  sz = t->family->sz;
  if (n > 0)
  {
    if (sz + n > last_slot_base)
    {
      return -1;
    }
    if ((sz = uvmalloc(t->family->pagetable, sz, sz + n, PTE_W)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(t->family->pagetable, sz, sz + n);
  }
  t->family->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int kfork(void)
{
  int i, fid;
  struct thread *nt;
  struct thread_family_shared *nf;
  struct thread *t = mythread();

  if ((nf = alloc_family()) == NULL)
  {
    return -1;
  }

  if ((nt = alloc_thread(nf)) == NULL)
  {
    free_family(nf);
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(t->family->pagetable, nf->pagetable, 0, t->family->sz) < 0)
  {
    free_thread(nt);
    free_family(nf);
    return -1;
  }

  uint64 dst_pa = walkaddr(nt->family->pagetable, USTACK_TOP(nt->slot_index) - PGSIZE);
  uint64 src_pa = walkaddr(t->family->pagetable, USTACK_TOP(t->slot_index) - PGSIZE);
  memmove((void *)dst_pa, (void *)src_pa, PGSIZE);

  nt->family->sz = t->family->sz;

  // copy saved user registers.
  *(nt->trapframe) = *(t->trapframe);

  // Cause fork to return 0 in the child.
  nt->trapframe->a0 = 0;

  uint64 parent_used = USTACK_TOP(t->slot_index) - t->trapframe->sp;
  nt->trapframe->sp = USTACK_TOP(nt->slot_index) - parent_used;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (t->family->ofile[i])
      nt->family->ofile[i] = filedup(t->family->ofile[i]);
  nt->family->cwd = idup(t->family->cwd);

  safestrcpy(nt->name, t->name, sizeof(t->name));

  fid = nf->fid;

  memmove(nt->family->vmas, t->family->vmas, sizeof(t->family->vmas));

  for (int i = 0; i < NVMA; i++)
  {
    struct vma v = nt->family->vmas[i];
    if (v.inode != 0)
    {
      idup(v.inode);
    }
  }

  acquire(&wait_lock);
  nt->family->parent_family = t->family;
  release(&wait_lock);

  acquire(&nt->lock);
  nt->state = RUNNABLE;
  if (nt->first_runnable_tick == -1)
    nt->first_runnable_tick = ticks;
  nt->last_runnable_tick = ticks;
  release(&nt->lock);

  return fid;
}

uint64 kclone(uint64 fn, uint64 arg)
{
  struct thread *myt = mythread();
  struct thread_family_shared *myf = myt->family;

  acquire(&myf->spinlk);
  if (myf->no_clone == 1)
  {
    release(&myf->spinlk);
    return -1;
  }
  release(&myf->spinlk);

  struct thread *nt;

  if ((nt = alloc_thread(myf)) == NULL)
  {
    return -1;
  }

  nt->trapframe->epc = fn;
  nt->trapframe->a0 = arg;

  acquire(&myf->spinlk);
  myf->tcount++;
  release(&myf->spinlk);

  acquire(&nt->lock);
  nt->state = RUNNABLE;
  int nt_tid = nt->tid;
  release(&nt->lock);

  return nt_tid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct thread_family_shared *f)
{
  struct thread_family_shared *ff;

  acquire(&family_list_lock);
  for (ff = init_family; ff != NULL; ff = ff->next)
  {
    if (ff->parent_family == f)
    {
      ff->parent_family = init_family;
      wakeup(init_family);
    }
  }
  release(&family_list_lock);
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void kexit(int status)
{
  struct thread *t = mythread();
  struct thread_family_shared *f = t->family;

  acquire(&f->spinlk);
  f->no_clone = 1;
  int siblings = f->tcount - 1;
  release(&f->spinlk);

  acquire(&thread_list_lock);
  for (struct thread *tt = init_thread; tt != NULL; tt = tt->next)
  {
    if (tt->family == f && tt->tid != t->tid)
    {
      kkill_thread(tt->tid, 0);
    }
  }
  release(&thread_list_lock);

  for (int i = 0; i < siblings; i++)
  {
    kjoin(0);
  }

  kexit_thread(status);
}

void kexit_thread(int status)
{
  struct thread *t = mythread();
  struct thread_family_shared *f = t->family;

  if (t == init_thread)
    panic("init exiting");

  acquire(&f->spinlk);
  f->tcount--;
  int thread_count = f->tcount;
  release(&f->spinlk);

  if (thread_count == 0)
  {
    acquiresleep(&f->sleeplk);
    for (int fd = 0; fd < NOFILE; fd++)
    {
      if (t->family->ofile[fd])
      {
        struct file *f = t->family->ofile[fd];
        fileclose(f);
        t->family->ofile[fd] = 0;
      }
    }

    for (int i = 0; i < NVMA; i++)
    {
      struct vma v = f->vmas[i];
      if (v.inode != 0)
      {
        iput(v.inode);
      }
    }
    memset(f->vmas, 0, sizeof(f->vmas));

    begin_op();
    iput(f->cwd);
    end_op();
    f->cwd = 0;

    releasesleep(&f->sleeplk);

    acquire(&wait_lock);

    // Give any children to init.
    reparent(f);

    // Parent might be sleeping in wait().
    wakeup(f->parent_family);

    acquire(&t->lock);

    f->xstate = status;
    t->state = ZOMBIE;
    if (t->exit_tick == -1)
      t->exit_tick = ticks;

    release(&wait_lock);
  }
  else
  {
    acquire(&wait_lock);

    wakeup(f);

    acquire(&t->lock);

    t->xstate = status;
    t->state = ZOMBIE;
    if (t->exit_tick == -1)
      t->exit_tick = ticks;

    release(&wait_lock);
  }

  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int kwait(uint64 addr)
{
  struct thread_family_shared *ff;
  int havekids, fid;
  struct thread_family_shared *f = myfamily();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    int needs_retry = 0;
    acquire(&family_list_lock);
    for (ff = init_family; ff != NULL; ff = ff->next)
    {
      acquire(&ff->spinlk);
      if (ff->parent_family == f)
      {
        // make sure the child isn't still in exit() or swtch().

        havekids = 1;

        if (ff->tcount == 0)
        {
          fid = ff->fid;
          int xstate = ff->xstate;
          release(&ff->spinlk);

          // Verify all threads are ZOMBIE before freeing. A thread may have
          // decremented tcount to 0 but not yet acquired wait_lock to set ZOMBIE.
          // Since we hold wait_lock, it's blocked — sleep to let it proceed.
          int ready = 1;
          acquire(&thread_list_lock);
          for (struct thread *tt = init_thread; tt != NULL; tt = tt->next)
          {
            if (tt->family == ff)
            {
              acquire(&tt->lock);
              if (tt->state != ZOMBIE)
                ready = 0;
              release(&tt->lock);
            }
          }
          release(&thread_list_lock);

          if (!ready)
          {
            release(&family_list_lock);
            sleep(f, &wait_lock); // releases wait_lock; dying thread proceeds → sets ZOMBIE → wakeup(f)
            needs_retry = 1;
            break;
          }

          acquire(&thread_list_lock);
          struct thread *tt = init_thread;
          while (tt != NULL)
          {
            struct thread *tnext = tt->next;
            if (tt->family == ff)
            {
              release(&thread_list_lock);
              free_thread(tt);
              acquire(&thread_list_lock);
              tt = init_thread;
            }
            else
            {
              tt = tnext;
            }
          }
          release(&thread_list_lock);

          if (addr != 0 && copyout(f->pagetable, addr, (char *)&xstate,
                                   sizeof(xstate)) < 0)
          {
            release(&family_list_lock);
            release(&wait_lock);
            return -1;
          }
          // Release both locks before free calls: free_family -> wakeup and
          // free_thread both try to acquire thread_list_lock.
          // wait_lock is still held so no concurrent kwait can race on this zombie.

          release(&family_list_lock);
          free_family(ff);
          release(&wait_lock);
          return fid;
        }
      }
      release(&ff->spinlk);
    }
    if (!needs_retry)
      release(&family_list_lock); // only release if inner loop ran to completion

    if (needs_retry)
      continue; // ← this continue is on the outer for(;;), restarts whole scan

    // No point waiting if we don't have any children.
    if (!havekids)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(f, &wait_lock); // DOC: wait-sleep
  }
}

int kjoin(uint64 addr)
{
  struct thread *tt;
  int havesiblings, tid;
  struct thread *t = mythread();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havesiblings = 0;
    acquire(&thread_list_lock);
    for (tt = init_thread; tt != NULL; tt = tt->next)
    {
      if (tt->state != UNUSED && tt->family != NULL && tt->family == t->family && tt->tid != t->tid)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&tt->lock);

        havesiblings = 1;
        if (tt->state == ZOMBIE)
        {
          // Found one.
          tid = tt->tid;
          if (addr != 0 && copyout(t->family->pagetable, addr, (char *)&tt->xstate,
                                   sizeof(tt->xstate)) < 0)
          {
            release(&tt->lock);
            release(&thread_list_lock);
            release(&wait_lock);
            return -1;
          }
          // Release both locks before free calls: free_family -> wakeup and
          // free_thread both try to acquire thread_list_lock.
          // wait_lock is still held so no concurrent kwait can race on this zombie.
          release(&tt->lock);
          release(&thread_list_lock);
          free_thread(tt);
          release(&wait_lock);
          return tid;
        }
        release(&tt->lock);
      }
    }
    release(&thread_list_lock);

    // No point waiting if we don't have any children.
    if (!havesiblings || killed(t))
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(t->family, &wait_lock); // DOC: wait-sleep
  }
}

int scheduler_type = MLFQ;

struct sched_state
{
  uint last_boost_tick;
  struct thread *chosen_proc;
};

struct sched_state cpu_sched_state[NCPU];

void sched_rr(struct cpu *c)
{
  acquire(&thread_list_lock);
  for (struct thread *t = init_thread; t != NULL; t = t->next)
  {
    acquire(&t->lock);
    if (t->state == RUNNABLE)
    {
      t->state = RUNNING;
      if (t->first_run_tick == -1)
        t->first_run_tick = ticks;
      t->runs_count += 1;
      t->total_wait_time += ticks - t->last_runnable_tick;
      c->thread = t;

      sfence_vma();
      release(&thread_list_lock);
      swtch(&c->context, &t->context);
      c->thread = 0;
      release(&t->lock);
      return;
    }
    release(&t->lock);
  }
  release(&thread_list_lock);
}

void sched_mlfq(struct cpu *c)
{
  struct sched_state *ss = &cpu_sched_state[cpuid()];
  static const int mlfq_allotment[NMLFQ] = {100, 200, 400, 800};
  int boost_now = 0;

  if (ticks - ss->last_boost_tick >= MLFQ_BOOST)
  {
    boost_now = 1;
  }

  int found = 0;
  acquire(&thread_list_lock);
  for (struct thread *t = init_thread; t != NULL; t = t->next)
  {
    acquire(&t->lock);
    if (boost_now == 1 && t->state != UNUSED)
    {
      t->cpu_time = 0;
      t->priority = 0;
    }

    if (t->state != UNUSED && t->cpu_time >= mlfq_allotment[t->priority])
    {
      if (t->priority < NMLFQ - 1)
      {
        t->priority += 1;
        t->cpu_time = 0;
      }
    }

    if (t->state == RUNNABLE &&
        (ss->chosen_proc == NULL || ss->chosen_proc->priority > t->priority))
    {
      if (ss->chosen_proc != NULL)
        release(&ss->chosen_proc->lock);
      ss->chosen_proc = t;
      found = 1;
    }
    else
    {
      release(&t->lock);
    }
  }

  if (boost_now == 1)
  {
    ss->last_boost_tick = ticks;
    boost_now = 0;
  }

  if (found == 1)
  {

    // Switch to chosen process.  It is the process's job
    // to release its lock and then reacquire it
    // before jumping back to us.
    ss->chosen_proc->state = RUNNING;

    // Observability calculations
    if (ss->chosen_proc->first_run_tick == -1)
      ss->chosen_proc->first_run_tick = ticks;

    int wait_time = ticks - ss->chosen_proc->last_runnable_tick;
    ss->chosen_proc->total_wait_time += wait_time;
    ss->chosen_proc->runs_count += 1;

    c->thread = ss->chosen_proc;

    sfence_vma();
    release(&thread_list_lock);
    swtch(&c->context, &ss->chosen_proc->context);

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->thread = 0;
    struct thread *chosen = ss->chosen_proc;
    ss->chosen_proc = NULL;
    release(&chosen->lock);
  }
  else
  {
    release(&thread_list_lock);
    // nothing to run; stop running on this core until an interrupt.
    asm volatile("wfi");
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void (*schedulers[])(struct cpu *) = {sched_rr, sched_mlfq};
void scheduler(void)
{
  struct cpu *c = mycpu();

  c->thread = 0;
  for (;;)
  {
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    schedulers[scheduler_type](c);
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct thread *p = mythread();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct thread *p = mythread();
  acquire(&p->lock);
  p->state = RUNNABLE;
  p->last_runnable_tick = ticks;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct thread *p = mythread();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    uint64 exec_result = kexec("/init", (char *[]){"/init", 0});
    p->trapframe->a0 = exec_result;
    if (p->trapframe->a0 == -1)
    {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->family->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct thread *t = mythread();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&t->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  t->chan = chan;
  t->state = SLEEPING;

  sched();

  // Tidy up.
  t->chan = 0;

  // Reacquire original lock.
  release(&t->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void wakeup(void *chan)
{
  acquire(&thread_list_lock);
  for (struct thread *t = init_thread; t != NULL; t = t->next)
  {
    if (t != mythread())
    {
      acquire(&t->lock);
      if (t->state == SLEEPING && t->chan == chan)
      {
        t->state = RUNNABLE;
        t->last_runnable_tick = ticks;
      }
      release(&t->lock);
    }
  }
  release(&thread_list_lock);
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kkill_thread(int tid, int holdlock)
{
  if (holdlock)
    acquire(&thread_list_lock);
  for (struct thread *t = init_thread; t != NULL; t = t->next)
  {
    acquire(&t->lock);
    if (t->tid == tid)
    {
      t->killed = 1;
      if (t->state == SLEEPING)
      {
        // Wake process from sleep().
        t->state = RUNNABLE;
        t->last_runnable_tick = ticks;
      }
      release(&t->lock);
      if (holdlock)
        release(&thread_list_lock);
      return 0;
    }
    release(&t->lock);
  }
  if (holdlock)
    release(&thread_list_lock);
  return -1;
}

int kkill(int fid)
{
  int found = 0;

  acquire(&thread_list_lock);
  for (struct thread *t = init_thread; t != NULL; t = t->next)
  {
    if (t->family != NULL && t->family->fid == fid)
    {
      found = 1;
      kkill_thread(t->tid, 0);
    }
  }
  release(&thread_list_lock);

  if (found == 0)
  {
    return -1;
  }
  else
  {
    return 0;
  }
}

void setkilled(struct thread *t)
{
  acquire(&t->lock);
  t->killed = 1;
  release(&t->lock);
}

int killed(struct thread *t)
{
  int k;

  acquire(&t->lock);
  k = t->killed;
  release(&t->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct thread *p = mythread();
  if (user_dst)
  {
    return copyout(p->family->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct thread *p = mythread();
  if (user_src)
  {
    return copyin(p->family->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct thread *t;
  char *state;

  printf("\n");
  acquire(&thread_list_lock);
  for (t = init_thread; t != NULL; t = t->next)
  {
    if (t->state == UNUSED)
      continue;
    if (t->state >= 0 && t->state < NELEM(states) && states[t->state])
      state = states[t->state];
    else
      state = "???";
    printf("%d %s %s", t->tid, state, t->name);
    printf("\n");
  }
  release(&thread_list_lock);
}
