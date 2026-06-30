#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

// ── helpers ───────────────────────────────────────────────────────────────────

static void die(const char *msg)
{
    printf("FAILED: %s\n", msg);
    exit(1);
}

void spin_fn(void *arg)  { for (;;) {} }
void noop_fn(void *arg)  { exit_thread(0); }

// ── test 1: slot exhaustion ───────────────────────────────────────────────────
// Fill all NFAMILY_THREADS - 1 sibling slots, verify the next clone fails
// gracefully (returns -1). Join all, verify slots are reclaimed.

void test_slot_exhaustion(void)
{
    printf("test_slot_exhaustion: ");

    int max_siblings = NFAMILY_THREADS - 1;
    for (int i = 0; i < max_siblings; i++)
    {
        if (clone(noop_fn, 0) < 0)
            die("clone failed before slots exhausted");
    }

    if (clone(noop_fn, 0) != -1)
        die("expected clone to fail when all slots full");

    for (int i = 0; i < max_siblings; i++)
    {
        if (join(0) < 0)
            die("join failed");
    }

    // Slots freed — must be able to clone again
    if (clone(noop_fn, 0) < 0)
        die("clone failed after slots freed");
    if (join(0) < 0)
        die("join failed after re-clone");

    printf("OK\n");
}

// ── test 2: guard page / stack overflow ──────────────────────────────────────
// Thread does infinite recursion with a large frame, hits the guard page,
// gets killed (status -1). Rest of the process is unaffected.

static volatile int keep_recursing = 1;

void overflow_fn(void *arg)
{
    volatile char buf[512] = {0}; // large frame to exhaust stack quickly
    buf[0] += 1;                  // read + write: suppress unused-variable warning
    if (keep_recursing)     // always true at runtime; compiler can't prove it
        overflow_fn(arg);
    exit_thread(0);         // unreachable in practice
}

void test_guard_page(void)
{
    printf("test_guard_page: ");

    int pid = fork();
    if (pid == 0)
    {
        clone(overflow_fn, 0);
        uint64 status;
        int tid = join(&status);
        if (tid < 0)
            die("join returned -1 after overflow");
        if ((int)status != -1)
        {
            printf("expected status -1 got %d\n", (int)status);
            exit(1);
        }
        exit(0);
    }

    int status;
    wait(&status);
    if (status != 0)
        die("guard page child failed");

    printf("OK\n");
}

// ── test 3: concurrent sbrk() ────────────────────────────────────────────────
// N threads each call sbrk(4096) in a loop while main does the same.
// Verifies no corruption or panic under concurrent address space growth.

#define SBRK_THREADS  4
#define SBRK_ITERS   16

void sbrk_fn(void *arg)
{
    for (int i = 0; i < SBRK_ITERS; i++)
    {
        if (sbrk(4096) == (char *)-1)
            exit_thread(1); // signal failure
    }
    exit_thread(0);
}

void test_concurrent_sbrk(void)
{
    printf("test_concurrent_sbrk: ");

    int pid = fork();
    if (pid == 0)
    {
        for (int i = 0; i < SBRK_THREADS; i++)
        {
            if (clone(sbrk_fn, 0) < 0)
                die("clone failed in sbrk test");
        }

        for (int i = 0; i < SBRK_ITERS; i++)
            sbrk(4096);

        for (int i = 0; i < SBRK_THREADS; i++)
        {
            uint64 status;
            if (join(&status) < 0)
                die("join failed in sbrk test");
            if ((int)status != 0)
                die("sbrk thread reported failure");
        }
        exit(0);
    }

    int status;
    wait(&status);
    if (status != 0)
        die("concurrent sbrk child failed");

    printf("OK\n");
}

// ── test 4: fork() from multi-threaded then exec() in child ──────────────────
// Multi-threaded process forks. Child comes out single-threaded (fork copies
// only the calling thread). Child execs echo (no siblings to kill in child).
// Spinning siblings in the parent are cleaned up when the parent exits.
//
// The whole test is wrapped in its own fork so exit(0) at the end cleans
// up the spinning threads via kexit without terminating the test suite.

void test_fork_exec(void)
{
    printf("test_fork_exec: ");

    int pid = fork(); // outer fork to isolate spinning threads
    if (pid == 0)
    {
        // Spawn spinning siblings in this process
        if (clone(spin_fn, 0) < 0) die("clone failed");
        if (clone(spin_fn, 0) < 0) die("clone failed");

        int cpid = fork(); // inner fork — child is single-threaded
        if (cpid == 0)
        {
            // Child: verify no siblings (fork must not copy parent's threads)
            if (join(0) != -1)
                die("child has unexpected siblings after fork");

            char *argv[] = {"echo", "fork_exec_ok", 0};
            exec("echo", argv);
            die("exec failed");
        }

        int status;
        wait(&status);
        if (status != 0)
            die("grandchild exec failed");

        // exit(0) → kexit() kills and joins the 2 spinning siblings
        exit(0);
    }

    int status;
    wait(&status);
    if (status != 0)
        die("fork_exec test failed");

    printf("OK\n");
}

// ── test 5: exit() from a non-main thread ────────────────────────────────────
// A cloned thread calls exit(77). kexit() must kill all other threads
// (including the spinning main thread), join them, and exit with status 77.

void exiter_fn(void *arg) { exit(77); }

void test_nonmain_exit(void)
{
    printf("test_nonmain_exit: ");

    int pid = fork();
    if (pid == 0)
    {
        clone(spin_fn, 0);
        clone(spin_fn, 0);
        clone(exiter_fn, 0);
        for (;;) {} // main thread spins; kexit() from exiter_fn will kill it
    }

    int status;
    wait(&status);
    if (status != 77)
    {
        printf("expected 77 got %d\n", status);
        exit(1);
    }
    printf("OK\n");
}

// ── test 6: leak detection (repeated clone/join cycles) ──────────────────────
// Repeatedly clone N threads and join them. Any leaked TCB, kstack, or slot
// will exhaust resources before all iterations complete.

#define LEAK_THREADS  4
#define LEAK_ITERS   100

void work_fn(void *arg) { exit_thread(0); }

void test_leak_detection(void)
{
    printf("test_leak_detection: ");

    for (int iter = 0; iter < LEAK_ITERS; iter++)
    {
        for (int i = 0; i < LEAK_THREADS; i++)
        {
            if (clone(work_fn, 0) < 0)
            {
                printf("clone failed at iter %d thread %d\n", iter, i);
                exit(1);
            }
        }
        for (int i = 0; i < LEAK_THREADS; i++)
        {
            if (join(0) < 0)
            {
                printf("join failed at iter %d\n", iter);
                exit(1);
            }
        }
    }

    printf("OK\n");
}

// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    test_slot_exhaustion();
    test_guard_page();
    test_concurrent_sbrk();
    test_fork_exec();
    test_nonmain_exit();
    test_leak_detection();
    printf("all stress tests passed\n");
    exit(0);
}
