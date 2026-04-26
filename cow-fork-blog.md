# Copy-on-Write Fork in xv6: Implementation, Bugs, and Benchmarks

*Full implementation on [GitHub](https://github.com/Amogh-Nivaskar/xv6-riscv/tree/cow-fork).*

## The Problem

Every time a Unix process calls `fork()`, the kernel duplicates the parent's entire address space. On a process with 4096 pages of memory, that's 4096 `kalloc()` calls and 4096 `memmove()` calls — 16MB of memory copied — before the child runs a single instruction.

This is wasteful in the common case. A shell forks a child for every command you type. That child immediately calls `exec()`, which throws away the copied address space and replaces it with a new binary. You paid 412 million CPU cycles to copy memory that was discarded before it was ever used.

The naive implementation doesn't just waste time — it wastes memory. On a 128MB machine, you can't fork a process larger than ~48MB because the kernel needs 2x the process size in free physical RAM. CoW eliminates that constraint by deferring copies until a write actually happens.

---

## The Naive Solution and Why It Fails

The original xv6 `uvmcopy()` is straightforward:

```c
pa = PTE2PA(*pte);
flags = PTE_FLAGS(*pte);
if ((mem = kalloc()) == 0)
    goto err;
memmove(mem, (char*)pa, PGSIZE);
mappages(new, i, PGSIZE, (uint64)mem, flags);
```

For every mapped page: allocate a new physical page, copy the contents, map it into the child's page table. The child gets a completely independent copy of every page — text, data, heap, stack — all of it, immediately.

The benchmarks show exactly what you'd expect from this. Without CoW, all four workloads cluster at ~415M cycles regardless of what the child actually does:

```
No-write fork (child exits immediately):  412M cycles
Write-all fork (child writes every page): 416M cycles
Write-1/4 fork (child writes 25% of pages): 417M cycles
Fork + exec (child execs a new binary):   422M cycles
```

That clustering is the signature of a system that pays the same cost regardless of actual work. The fork is doing 16MB of copying for a child that writes nothing, for a child that writes everything, and for a child that immediately calls exec. The cost is completely decoupled from the work.

```
Parent page table          Child page table
┌─────────────┐           ┌─────────────┐
│ VA → PA_1   │           │ VA → PA_1'  │  (copy)
│ VA → PA_2   │           │ VA → PA_2'  │  (copy)
│ VA → PA_3   │           │ VA → PA_3'  │  (copy)
└─────────────┘           └─────────────┘
     PA_1 ──────────────────► PA_1' (duplicate)
     PA_2 ──────────────────► PA_2' (duplicate)
```

---

## Design Decisions

### Ref Counting

When two processes share a physical page, the kernel needs to know when it's safe to free it — only when no process references it anymore. The solution is a reference count per physical page.

I used a global array indexed by physical address:

```c
int refcnt[(PHYSTOP - KERNBASE) / PGSIZE];
// index: (pa - KERNBASE) / PGSIZE
```

The key sizing decision: index by `(pa - KERNBASE) / PGSIZE`, not `pa / PGSIZE`. Physical addresses below `KERNBASE` (0x80000000) are device memory — the allocator never touches them. Using `pa / PGSIZE` wastes 2MB of kernel memory on indices that are never valid. With the correct offset, the array covers only the 32768 pages the allocator actually manages, costing ~128KB.

Lock discipline: `incref` and `decref` acquire `kmem.lock`. During boot, `freerange()` sets each page's refcount to 1 before calling `kfree()`, so the decrement in `kfree()` brings it to 0 and adds the page to the free list cleanly.

### The PTE_COW Bit

RISC-V PTEs have two RSW (Reserved for Software) bits at positions 8 and 9. xv6 uses neither, so bit 8 is free:

```c
#define PTE_COW (1L << 8)
```

This bit distinguishes "read-only because it's a CoW shared page" from "genuinely read-only" (text, rodata). That distinction matters: text pages should never be CoW'd. A process can't write to its own code, so there's no write fault to handle, and `copyout()` (which writes to user memory from the kernel) must not treat text pages as writable CoW pages.

**Only pages with `PTE_W` set get `PTE_COW`**. Read-only pages are shared as-is between parent and child with their original flags unchanged.

### The refcnt == 1 Optimization

When a write fault occurs on a CoW page and the ref count is already 1 — meaning this process is the sole owner — there's no need to copy. Just clear `PTE_COW`, set `PTE_W`, and flush the TLB:

```c
if (getref(pa) == 1) {
    *pte = (*pte & ~PTE_COW) | PTE_W;
    sfence_vma();
}
```

This handles the case where a parent forks, the child exits without writing to a particular page, and then the parent writes to it. Refcount drops to 1 on child exit; the parent's next write skips the copy entirely.

### copyout() Must Be CoW-Aware

`copyout()` transfers data from kernel space to user virtual addresses. It does this by walking the page table to get the physical address, then calling `memmove()` directly. The MMU is never involved — there's no page fault mechanism in play.

Without explicit CoW handling in `copyout()`, a kernel write to a CoW-shared page corrupts both the parent and child silently. The fix is to check `PTE_COW` at the start of each iteration and perform the same copy-and-remap logic before the `memmove`.

---

## The Implementation

### Fork: uvmcopy()

The full-copy loop becomes a share-and-mark loop:

```c
flags = PTE_FLAGS(*pte);
if (*pte & PTE_W) {
    flags &= ~PTE_W;
    flags |= PTE_COW;
}
pa = PTE2PA(*pte);
if (mappages(new, i, PGSIZE, (uint64)pa, flags) != 0)
    goto err;
incref(pa);
*pte = PA2PTE(pa) | flags;  // update parent PTE too
```

No `kalloc()`. No `memmove()`. Both page tables point to the same physical page. Both have `PTE_W` cleared (to trigger a fault on write) and `PTE_COW` set (so the fault handler knows it's a CoW page, not a genuine protection violation).

The parent's PTE must be updated too — otherwise the parent can still write to its "writable" pages while the child shares them.

### Write Fault: usertrap()

A write to a CoW page triggers a store page fault (`scause = 15`). The handler:

```c
} else if (r_scause() == 15) {
    uint64 va = PGROUNDDOWN(r_stval());
    if (va >= MAXVA) {
        setkilled(p);
    } else {
        pte_t *pte = walk(p->pagetable, va, 0);
        if (pte && (*pte & PTE_COW) && (*pte & PTE_U)) {
            uint64 pa = PTE2PA(*pte);
            if (getref(pa) == 1) {
                *pte = (*pte & ~PTE_COW) | PTE_W;
                sfence_vma();
            } else {
                char *mem = kalloc();
                if (mem == 0) { setkilled(p); }
                else {
                    memmove(mem, (char*)pa, PGSIZE);
                    uint flags = (PTE_FLAGS(*pte) & ~PTE_COW) | PTE_W;
                    *pte = PA2PTE((uint64)mem) | flags;
                    kfree((void*)pa);
                    sfence_vma();
                }
            }
        } else {
            // Not a CoW page — try lazy allocation, then kill
            if (vmfault(p->pagetable, va, 0) == 0)
                setkilled(p);
        }
    }
}
```

The `va >= MAXVA` check is required before calling `walk()`. Without it, a process that faults at an address beyond MAXVA causes `walk()` to panic and halt the entire kernel — not just kill the offending process.

One non-obvious piece: the `vmfault()` fallback in the else branch. The lazy allocator also uses `scause = 15` (a write to an unmapped page). Without this fallback, any unallocated page write would hit `setkilled` instead of triggering lazy allocation.

---

## Benchmarks and What They Mean

**Methodology**: xv6 on QEMU (`-smp 3, -m 128M`), measured in CPU cycles via the RISC-V `rdcycle` CSR (enabled via `scounteren`), 500 trials averaged, 4096 pages (16MB) pre-allocated and dirtied before each test loop.

| Test | Without CoW | With CoW | Delta |
|------|-------------|----------|-------|
| No-write fork | 412M cycles | 23M cycles | **18x faster** |
| Write-all fork | 416M cycles | 790M cycles | 1.9x slower |
| Write 1/4 of pages | 417M cycles | 222M cycles | **1.9x faster** |
| Fork + exec | 422M cycles | 38M cycles | **11x faster** |
| Max forkable size | ~48MB | ~80MB | **67% more** |

**The counterintuitive result**: write-all CoW is 1.9x *slower* than full-copy write-all. This one is worth sitting with. Full copy pays for 4096 page copies upfront in a tight kernel loop — sequential memory access, no trap overhead. CoW spreads those same 4096 copies across 4096 individual page faults, each requiring a full trap into the kernel, a page table walk, a `kalloc()`, a `memmove()`, a `kfree()`, a TLB flush, and a trap return. The per-fault overhead dominates.

CoW is a pessimization for workloads where the child writes to every page. It's an optimization for everything else.

**Fork + exec** confirms the design intent. At 38M cycles, it's only 65% more expensive than a pure no-write fork. The child execs before triggering most CoW faults — the copied page table entries are discarded by `exec()` before writes happen. This is the shell pattern. This is why CoW exists.

**Memory capacity** is the clearest argument. Without CoW, forking a 48MB process on a 128MB machine leaves you at the limit — the kernel needs 48MB for the parent and 48MB for the copy, plus overhead. CoW allows an 80MB process to be forked on the same machine. On production systems with gigabyte-scale processes, this isn't a nice-to-have.

---

## What's Next

The current implementation uses `kmem.lock` for all ref count operations. On a multicore machine, concurrent forks serialize on that single lock — every `incref()` during `uvmcopy()` contends with every `kfree()` from exiting processes. A production implementation would use per-page or per-slab locks to reduce contention as core count scales.

Beyond that: huge page support (a fault on a 2MB page shouldn't necessarily copy the full 2MB — you want to split it first), NUMA-aware page placement after copy (the new page should land on the same NUMA node as the faulting CPU), and KSM-style page deduplication — the logical inverse of CoW, where the kernel periodically merges identical pages across unrelated processes so they share physical memory until one writes.

The xv6 implementation covers the core mechanism cleanly. The rest is engineering for scale.
