#include "sleeplock.h"

// Saved registers for kernel context switches.
struct context
{
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
struct cpu
{
  struct thread *thread;  // The process running on this cpu, or null.
  struct context context; // swtch() here to enter scheduler().
  int noff;               // Depth of push_off() nesting.
  int intena;             // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
// the trapframe includes callee-saved user registers like s0-s11 because the
// return-to-user path via usertrapret() doesn't return through
// the entire kernel call stack.
struct trapframe
{
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

enum threadstate
{
  UNUSED,
  USED,
  SLEEPING,
  RUNNABLE,
  RUNNING,
  ZOMBIE
};

struct vma
{
  struct inode *inode;
  uint64 vaddr;
  uint64 off;
  uint64 filesz;
  uint64 memsz;
  uint32 flags;
};

// ---------------------------------------------------------THREADS-------------------------------------

struct thread
{
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

  char name[16];

  // Scheduling fields
  int priority; // current queue level
  int cpu_time; // ticks accumulated at current level

  // Observatory fields
  int total_wait_time;
  int runs_count;
  int first_runnable_tick;
  int last_runnable_tick;
  int first_run_tick;
  int exit_tick;
};

extern struct spinlock thread_list_lock;

extern struct thread *init_thread;

struct family_shared
{
  struct sleeplock sleeplk;
  struct spinlock spinlk;

  pagetable_t pagetable;
  uint64 sz;

  struct file *ofile[NOFILE];
  struct inode *cwd;
  struct vma vmas[NVMA];

  struct family_shared *parent_family;
  struct family_shared *next;
  int fid;

  int xstate;

  int tcount;
  int no_clone;
  int slot_tracking[NFAMILY_THREADS];
  uint64 heap_reserve;
};

extern struct spinlock family_list_lock;

extern struct family_shared *init_family;

extern int scheduler_type;
