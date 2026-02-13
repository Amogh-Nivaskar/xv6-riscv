# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

xv6-riscv is a teaching operating system (MIT 6.1810) implementing Unix V6 concepts on RISC-V. The kernel is ~5.3K lines of C and the user space is ~5.1K lines.

## Build and Run Commands

```bash
make qemu          # Build everything and run in QEMU (3 CPUs, 128MB RAM)
make qemu-gdb      # Build and run with GDB debugging support
make clean          # Remove all build artifacts
```

**Testing:**
```bash
./test-xv6.py usertests      # Run full test suite (~68 tests)
./test-xv6.py -q usertests   # Run quick subset of tests
./test-xv6.py crash           # Run crash recovery tests
```

Inside a running xv6 instance, you can run `usertests` or `usertests -q` directly from the shell.

**Toolchain:** Requires RISC-V GCC cross-compiler (`riscv64-unknown-elf-gcc` or variants) and `qemu-system-riscv64` (>= 7.2). The Makefile auto-detects the toolchain prefix.

**Compiler flags:** `-Wall -Werror` is enforced. Code is compiled with `-ffreestanding -nostdlib -mcmodel=medany -march=rv64gc`.

## Architecture

### Boot Sequence
1. QEMU loads kernel at `0x80000000` in machine mode
2. `kernel/entry.S` — sets up per-CPU stacks, calls `start()`
3. `kernel/start.c` — configures machine mode, transitions to supervisor mode, calls `main()`
4. `kernel/main.c` — hart 0 initializes all subsystems then enters `scheduler()`; other harts wait, then initialize their own paging/traps and enter `scheduler()`

### Kernel Subsystems (kernel/)

| Subsystem | Key Files | Description |
|-----------|-----------|-------------|
| Process mgmt | `proc.c`, `proc.h` | Process table (64 max), scheduling, fork/exec/exit/wait, context switching |
| Traps | `trap.c`, `trampoline.S`, `kernelvec.S` | User/kernel trap handling, interrupt dispatch |
| Virtual memory | `vm.c`, `memlayout.h` | Page tables, user/kernel address spaces, page allocation |
| Syscalls | `syscall.c`, `sysfile.c`, `sysproc.c` | 21 syscalls; dispatcher + argument fetching |
| Filesystem | `fs.c`, `log.c`, `bio.c` | Inode-based FS with transaction logging for crash recovery |
| Files/pipes | `file.c`, `pipe.c` | File descriptor table, pipe implementation |
| Memory alloc | `kalloc.c` | Physical page allocator (free list of 4KB pages) |
| Devices | `uart.c`, `virtio_disk.c`, `plic.c`, `console.c` | UART console, VirtIO disk, PLIC interrupt controller |
| Sync | `spinlock.c`, `sleeplock.c` | Spinlocks (interrupt-safe) and sleep locks (blocking) |
| Boot/init | `entry.S`, `start.c`, `main.c` | Machine-mode setup, kernel initialization |
| Context switch | `swtch.S` | Register save/restore for process switching |

### User Space (user/)

- `init.c` — first user process, spawns shell on console
- `sh.c` — shell with pipes and I/O redirection
- Standard utilities: `cat`, `echo`, `grep`, `ls`, `mkdir`, `rm`, `ln`, `wc`, `kill`
- `usertests.c` — comprehensive test suite
- `ulib.c`, `umalloc.c`, `printf.c` — user-space library (linked into all programs)
- `usys.pl` — Perl script that generates syscall stubs (`usys.S`) at build time
- `user.ld` — linker script for user programs

### Adding a New Syscall

1. Add `SYS_name` constant to `kernel/syscall.h`
2. Add handler entry to `syscalls[]` table in `kernel/syscall.c`
3. Implement `sys_name()` in `kernel/sysfile.c` or `kernel/sysproc.c`
4. Add entry to `user/usys.pl` to generate the user-space stub
5. Add prototype to `user/user.h`

### Adding a New User Program

1. Create `user/progname.c` with `main()` function
2. Add `$U/_progname` to the `UPROGS` list in the Makefile

### Key Constants (kernel/param.h)

NPROC=64 (max processes), NCPU=8 (max CPUs), NOFILE=16 (open files/process), FSSIZE=2000 (FS blocks), MAXPATH=128, MAXARG=32.

### Memory Layout

- **Physical**: Kernel at `0x80000000`, devices memory-mapped below that (UART at `0x10000000`, VirtIO at `0x10001000`, PLIC at `0x0C000000`)
- **User virtual**: text → data → BSS → heap (grows up via sbrk) → ... → stack → trapframe → trampoline (at MAXVA)
- **Kernel virtual**: identity-mapped physical memory + per-process kernel stacks below trampoline

### Filesystem Disk Layout

`[boot block][superblock][log blocks][inode blocks][bitmap][data blocks]`

Created by `mkfs/mkfs.c` which builds `fs.img` containing the README and all user programs.
