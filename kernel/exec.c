#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

// static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
  int perm = 0;
  if (flags & 0x1)
    perm = PTE_X;
  if (flags & 0x2)
    perm |= PTE_W;
  return perm;
}

//
// the implementation of the exec() system call
//
int kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct vma vmas[NVMA] = {0};
  struct thread *t = mythread();
  struct family_shared *f = t->family;
  int nvma = 0;
  int old_slotIdx = t->slot_index;
  struct trapframe *old_trapframe = t->trapframe;
  int siblings = 0;

  begin_op();

  // Open the executable file.
  if ((ip = namei(path)) == 0)
  {
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header.
  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if (elf.magic != ELF_MAGIC)
    goto bad;

  if ((pagetable = family_pagetable()) == 0)
    goto bad;

  // Load program into memory.
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
  {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if (ph.vaddr % PGSIZE != 0)
      goto bad;
    if (nvma >= NVMA)
      goto bad;

    struct vma v = {
        .inode = ip,
        .vaddr = ph.vaddr,
        .off = ph.off,
        .filesz = ph.filesz,
        .memsz = ph.memsz,
        .flags = ph.flags};
    vmas[nvma++] = v;
    idup(ip);

    sz = ph.vaddr + ph.memsz > sz ? ph.vaddr + ph.memsz : sz;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  uint64 oldsz = t->family->sz;

  sz = PGROUNDUP(sz);

  t->family->slot_tracking[old_slotIdx] = 0;

  if (alloc_slot(t, pagetable) == -1)
  {
    t->family->slot_tracking[old_slotIdx] = 1;
    goto bad;
  }

  sp = USTACK_TOP(t->slot_index);
  stackbase = sp - USERSTACK * PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for (argc = 0; argv[argc]; argc++)
  {
    if (argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if (sp < stackbase)
      goto bad;
    if (copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase)
    goto bad;
  if (copyout(pagetable, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  t->trapframe->a1 = sp;

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(t->name, last, sizeof(t->name));

  for (int j = 0; j < NVMA; j++)
  {
    if (t->family->vmas[j].inode != 0)
    {
      iput(t->family->vmas[j].inode);
      t->family->vmas[j].inode = 0;
    }
  }

  // All failure-prone operations are done. Now it's safe to kill siblings:
  // if exec had failed above, they would still be alive and the process
  // would continue normally. From here there are no more failure modes.
  acquire(&f->spinlk);
  f->no_clone = 1;
  siblings = f->tcount - 1;
  release(&f->spinlk);

  if (siblings > 0)
  {
    acquire(&thread_list_lock);
    for (struct thread *tt = init_thread; tt != 0; tt = tt->next)
      if (tt->family == f && tt->tid != t->tid)
        kkill_thread(tt->tid, 0);
    release(&thread_list_lock);

    for (int i = 0; i < siblings; i++)
      kjoin(0);
  }

  // Commit to the user image.
  oldpagetable = t->family->pagetable;
  t->family->pagetable = pagetable;
  memmove(t->family->vmas, vmas, sizeof(vmas));
  t->family->sz = sz;
  t->family->heap_reserve = sz + HEAP_RESERVE_PAGES * PGSIZE;
  t->trapframe->epc = elf.entry; // initial program counter = ulib.c:start()
  t->trapframe->sp = sp;         // initial stack pointer
  unmap_slot(old_slotIdx, oldpagetable, 1);
  family_freepagetable(oldpagetable, oldsz);

  // New program starts single-threaded; allow cloning again.
  acquire(&f->spinlk);
  f->no_clone = 0;
  release(&f->spinlk);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

bad:
  if (t->trapframe != old_trapframe)
  {
    unmap_slot(t->slot_index, pagetable, 1);
    t->slot_index = old_slotIdx;
    t->trapframe = old_trapframe;
  }

  if (pagetable)
    family_freepagetable(pagetable, sz);

  if (ip)
  {
    for (int j = 0; j < NVMA; j++)
    {
      if (vmas[j].inode != 0)
      {
        iput(vmas[j].inode);
        vmas[j].inode = 0;
      }
    }
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
// static int
// loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
// {
//   uint i, n;
//   uint64 pa;

//   for(i = 0; i < sz; i += PGSIZE){
//     pa = walkaddr(pagetable, va + i);
//     if(pa == 0)
//       panic("loadseg: address should exist");
//     if(sz - i < PGSIZE)
//       n = sz - i;
//     else
//       n = PGSIZE;
//     if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
//       return -1;
//   }

//   return 0;
// }
