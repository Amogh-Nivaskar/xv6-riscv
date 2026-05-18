# Lazy ELF Loading in xv6: Demand Paging for Executables

*Full implementation on [GitHub](https://github.com/Amogh-Nivaskar/xv6-riscv/tree/lazy-exec).*

---

## The Problem

When `exec()` loads a binary in xv6, it eagerly allocates pages and reads every ELF segment from disk before the process runs a single instruction. Running `fatbin` — a binary with 50 pages of initialized data — costs 516 million cycles per exec, almost all of it in `loadseg()` reading pages the process will never touch before calling `exit()`. The process uses maybe three pages of code during its lifetime. xv6 reads 50 pages of data regardless.

The naive implementation doesn't just waste time. Every `exec()` is a bulk disk read proportional to binary size, not to what the binary actually uses. On a machine with many short-lived processes — a shell, a build system, anything that spawns frequently — you pay the entire library price to load a single function from it.

---

## The Naive Solution and Why It Fails

The original `exec()` loops through ELF program headers and for each loadable segment calls two functions before the process starts:

```c
if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
    goto bad;
if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
    goto bad;
```

The assumption is that the process needs all of this immediately. `uvmalloc()` allocates physical pages and installs PTEs with `PTE_V=1`. `loadseg()` then walks the page table, finds each physical page, and reads the corresponding file data into it. By the time `exec()` returns, every ELF page is resident in RAM.

This assumption breaks for any program that doesn't use its entire binary. A binary with 50 pages of initialized data loads all 50 pages even if it exits without reading a single byte of that data. Measured: 516M cycles spent in `exec()` for pages that are never accessed. The cost is entirely front-loaded — you pay it before the process is useful.

The worst workload is high-frequency exec of fat binaries. Each invocation reads the full binary off disk. The pages may be evicted by the next invocation's disk reads. Nothing is amortized.

---

## Design Decisions

**Recording VMAs instead of allocating pages**

The obvious approach would be to skip `loadseg()` but still call `uvmalloc()` — keep the PTEs installed but leave pages unpopulated. Simpler to wire up: the fault handler knows the VA, it just fills in the page. I rejected it because `uvmalloc()` still makes the `kalloc()` call per page. You defer the disk read but not the physical memory allocation. `exec()` time improves slightly; memory footprint at startup doesn't.

The better approach: don't call `uvmalloc()` or `loadseg()` at all. Instead, record each ELF segment as a VMA (virtual memory area) — a struct storing the inode, file offset, `filesz`, `memsz`, and ELF permission flags. Install no PTEs. When a page fault fires on a VMA address, allocate exactly one physical page, read exactly the bytes for that VA, and map it.

```c
struct vma {
  struct inode *inode;
  uint64 vaddr;
  uint64 off;
  uint64 filesz;
  uint64 memsz;
  uint32 flags;
};
```

Each process gets up to `NVMA=8` slots in its `struct proc` — enough for any realistic ELF (text, data, and a few more). The tradeoff accepted: a linear scan of up to 8 entries on every ELF fault to find the owning segment.

**Inode lifetime**

`exec()` normally closes the file after loading ELF. With lazy loading, the inode must stay alive until the process exits or re-execs. For each VMA, `exec()` calls `idup()` to increment the reference count. `freeproc()` and the old-image cleanup path in `exec()` call `iput()` per VMA. Fork copies all VMAs and calls `idup()` again for each live inode — the child can independently demand-load its own pages. This is the same reference-counting pattern xv6 uses everywhere for inodes, applied to process lifetime.

**The boundary page**

ELF binaries have `filesz < memsz` for the data segment — the gap is BSS (zero-initialized). The boundary between `filesz` and `memsz` often falls in the middle of a 4KB page. That page needs the first N bytes read from the file and the remaining 4096−N bytes zeroed. Getting the byte arithmetic wrong produces corrupted BSS — a global variable initialized to zero reads back as garbage on first access. There are three cases to handle per fault, not two.

**Instruction fetch faults**

xv6's lazy heap allocator handles only `scause=13` (load page fault). Text pages trigger `scause=12` (instruction fetch fault) when the CPU tries to fetch an instruction from an unmapped page. Adding `r_scause() == 12` to the condition in `usertrap()` covers both. Without it, the process kills itself the first time it tries to execute code in an unloaded text page.

**Kernel-side accesses via `walkaddr`**

`exec()` passes argv strings through the user address space. `copyinstr()` calls `walkaddr()` to translate user VAs to physical addresses. If the argv page hasn't been faulted in yet, `walkaddr()` returns 0 and `exec()` fails with −1. The fix: if `walkaddr()` finds no valid PTE, attempt `vmfault()` first, then retry the walk. This turns `walkaddr()` from a pure lookup into a lookup-or-fault function for the user address space.

---

## The Implementation

The core of lazy ELF loading lives in `vmfault()`, which xv6 already used for lazy heap allocation. On a fault, the handler first checks whether the VA belongs to a VMA:

```c
int vmaIdx = -1;
for(int i = 0; i < NVMA; i++){
    struct vma v = p->vmas[i];
    if(va >= v.vaddr && va < v.vaddr + v.memsz){
        vmaIdx = i;
        break;
    }
}
```

If `vmaIdx == -1`, fall through to the original zero-fill heap path. For a VMA fault, read the appropriate file data into the freshly allocated page:

```c
uint64 offset = v.off + (va - v.vaddr);
ilock(v.inode);
if(va + PGSIZE <= v.vaddr + v.filesz){
    readi(v.inode, 0, (uint64)mem, offset, PGSIZE);
} else if(va >= v.vaddr + v.filesz){
    memset((void *)mem, 0, PGSIZE);
} else {
    uint64 textsz = v.vaddr + v.filesz - va;
    readi(v.inode, 0, (uint64)mem, offset, textsz);
    memset((void *)mem + textsz, 0, PGSIZE - textsz);
}
iunlock(v.inode);
flags = flags2perm(v.flags) | PTE_R | PTE_U;
```

The permissions come from the ELF program header flags, not a hardcoded constant. Text pages get `PTE_R|PTE_X` but not `PTE_W` — a write to a code page kills the process. Using a hardcoded `PTE_W|PTE_R|PTE_U` would silently make text writable, which the `test_text_readonly` test catches.

The non-obvious constraint: `ilock()` is held across the `readi()` call, which can sleep waiting for disk I/O. The process lock must not be held during this sleep — xv6's locking discipline prohibits a process from holding its own lock while sleeping. The order is: acquire ilock, read, release ilock, then call `mappages()`.

---

## Benchmarks

All measurements on xv6-riscv in QEMU (3 CPUs, 128MB RAM). Timing via `rdcycle` CSR, enabled with `scounteren.CY=1` in machine mode during startup. Exec latency: N=100 trials of `fork + exec + wait`. Memory footprint: N=10 processes simultaneously, measured via `countfree()` before and after.

The benchmark binary is purpose-built. `fatbin` carries `char bigdata[50 * 4096] = {1}` — 50 pages of initialized data in the ELF file. Eager exec reads all 50 pages on every exec. Lazy exec records a VMA and returns; since the process exits without touching the array, those pages are never loaded. The `= {1}` initializer matters: plain `char bigdata[...]` goes to BSS and has no file presence, so eager exec wouldn't read it either.

```
Metric                         | Eager      | Lazy       | Delta
-------------------------------|------------|------------|------------
exec latency (fork+exec+wait)  | 516M cyc   | 20.6M cyc  | 25x faster
pages/process at startup       | 50 pages   | 9 pages    | 5.5x less
first function call            | 147K cyc   | 195K cyc   | 1.3x slower
warm function call             | ~3K cyc    | ~3K cyc    | same
```

The exec latency result is decisive: 25x faster because lazy exec reads zero ELF pages from disk during `exec()`. The 20.6M cycles are fork + process table setup + wait — the disk I/O is gone entirely.

Memory footprint shows 5.5x less physical memory per process at startup. Eager loads all 50 data pages immediately. Lazy loads only pages actually touched: stack, a few text pages for `main()` and the libc wrappers. Nine pages instead of fifty.

The counterintuitive result: lazy first-call at 195K cycles is **slower** than eager first-call at 147K cycles. That's the fault overhead: trap into kernel, VMA scan, `ilock`, `readi`, `mappages`, return to userspace. You've deferred that cost from `exec()` to first access — but it still exists. On eager exec the page is already resident and call 1 costs the same as call 2. Lazy exec trades predictable startup cost for per-page fault jitter. On programs that touch most of their binary, lazy exec is a pessimization: you pay the same disk I/O, spread across N individual faults, plus N trap overheads.

Lazy loading is an optimization for programs that don't use their entire binary. That's most programs.

---

## What I'd Do Differently / What's Next

My first implementation didn't handle the boundary page case — the page where `filesz` cuts through a 4KB boundary. Pure BSS and full-file pages worked. The boundary page returned garbage because I read `PGSIZE` bytes starting at the file offset, but the file only had `textsz` bytes there. BSS variables in the same page came back non-zero. `test_bss()` caught it; I wouldn't have spotted it from inspection.

The fault handler currently holds `ilock()` across the entire disk read. In a multi-threaded process, this serializes all ELF page faults on the same binary through one inode lock. A production implementation would snapshot the block addresses needed, drop the ilock, read from the buffer cache, then re-lock and validate before calling `mappages()`.

The VMA lookup is an O(NVMA) linear scan per fault. With NVMA=8 it's invisible, but the right structure for a larger system is an interval tree keyed on `(vaddr, vaddr + memsz)` — O(log N) lookup regardless of segment count.

Next: swapping. The per-page VMA infrastructure is exactly what eviction needs — a clean mapping from VA back to the file and offset that produced it. The open question is policy: which page do you write back to disk when physical memory is full?
