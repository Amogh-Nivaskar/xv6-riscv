#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "procinfo.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_hello(void){
  printf("Hello from kernel!\n");
  return 0;
}

uint64
sys_getproccount(void){
  int count = 0;
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++){
	if (p->state != UNUSED){
		count++;
	}
  }

  return count;
}

uint64
sys_getprocinfo(void){
  uint64 st;
  struct procinfo pi;
  struct proc *p;
  int count = 0;

  argaddr(0, &st);
  for (p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if (p->state != UNUSED){
      pi.entries[count].pid = p->pid;
      pi.entries[count].cpu_time = p->cpu_time;
      pi.entries[count].priority = p->priority;
      pi.entries[count].state = p->state;
      pi.entries[count].total_wait_time = p->total_wait_time;
      pi.entries[count].runs_count = p->runs_count;
      pi.entries[count].runnable_tick = p->runnable_tick;
      safestrcpy(pi.entries[count].name, p->name, 16);
      count++;
    }
    release(&p->lock);
  } 
  pi.count = count;
  return copyout(myproc()->pagetable, st, (char*)&pi, sizeof(pi));
}

uint64
sys_settracer(void){
  extern int tracer_enabled;
  int value;
  argint(0, &value);
  tracer_enabled = value;
  return 0;
}

uint64 sys_sleep(void){
  extern uint ticks;
  int duration;
  argint(0, &duration);
  
  acquire(&tickslock);
  int start_tick = ticks;
  while (ticks - start_tick < duration){
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);

  return 0;
};

uint64 sys_setscheduler(void){
  int mode;
  argint(0, &mode);

  if (mode < 0 || mode >= SCHED_COUNT){
    return -1;
  }    

  scheduler_type = mode;
  return 0;
}