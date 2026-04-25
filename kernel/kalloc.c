// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

int refcnt[(PHYSTOP - KERNBASE)/PGSIZE];

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    refcnt[PA2IDX((uint64)p)] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.lock);

  int idx = PA2IDX(pa);

  if (refcnt[idx] <= 0)
    panic("kfree: refcnt already zero");

  refcnt[idx]--;

  if (refcnt[idx] > 0) {
    release(&kmem.lock);
    return;
  }

  memset(pa, 1, PGSIZE);

  struct run *r = (struct run*)pa;
  r->next = kmem.freelist;
  kmem.freelist = r;

  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    refcnt[PA2IDX(r)] = 1;
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void incref(uint64 pa){
  acquire(&kmem.lock);
  refcnt[PA2IDX(pa)]++;
  release(&kmem.lock);
}

void decref(uint64 pa){
  acquire(&kmem.lock);

  int idx = PA2IDX(pa);
  if (refcnt[idx] <= 0)
    panic("decref: refcnt already zero");

  refcnt[idx]--;

  release(&kmem.lock);
}

int getref(uint64 pa){
  acquire(&kmem.lock);
  int val = refcnt[PA2IDX(pa)];
  release(&kmem.lock);
  return val;
}