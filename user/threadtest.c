#include "kernel/types.h"
#include "user/user.h"

// ── helpers ──────────────────────────────────────────────────────────────────

void thread_fn(void *arg)
{
    int id = (int)(uint64)arg;
    printf("  thread %d running\n", id);
    exit_thread(id * 10);
}

void spin_fn(void *arg)
{
    for (;;) {}
}

void noop_fn(void *arg)
{
    exit_thread(0);
}

// ── test 1: basic clone / exit_thread / join ─────────────────────────────────

void test_basic(void)
{
    printf("test_basic: ");
#define N 3
    for (int i = 0; i < N; i++)
    {
        if (clone(thread_fn, (void *)(uint64)i) < 0)
        {
            printf("clone failed\n");
            exit(1);
        }
    }
    for (int i = 0; i < N; i++)
    {
        int status;
        if (join((uint64 *)&status) < 0)
        {
            printf("join failed\n");
            exit(1);
        }
    }
    printf("OK\n");
}

// ── test 2: exit() from multi-threaded kills siblings ────────────────────────
// Fork a child that spawns spinning threads then calls exit(42).
// kexit() must kill + join siblings before exiting.
// Parent checks the wait status is 42.

void test_exit(void)
{
    printf("test_exit: ");
    int pid = fork();
    if (pid == 0)
    {
        clone(spin_fn, 0);
        clone(spin_fn, 0);
        exit(42);
    }
    int status;
    wait(&status);
    if (status != 42)
    {
        printf("FAILED (status=%d expected 42)\n", status);
        exit(1);
    }
    printf("OK\n");
}

// ── test 3: fork() from multi-threaded — child is single-threaded ─────────────
// Parent clones a thread, then forks.
// Child must have no siblings: join() should return -1 immediately.
// Parent joins its cloned thread, then waits for child.

void test_fork(void)
{
    printf("test_fork: ");
    clone(noop_fn, 0);

    int pid = fork();
    if (pid == 0)
    {
        // join() with no siblings must return -1, not hang
        int r = join(0);
        if (r != -1)
        {
            printf("child unexpectedly has siblings (join=%d)\n", r);
            exit(1);
        }
        exit(0);
    }

    join(0); // collect noop thread in parent

    int status;
    wait(&status);
    if (status != 0)
    {
        printf("FAILED (child status=%d)\n", status);
        exit(1);
    }
    printf("OK\n");
}

// ── test 4: exec() from multi-threaded kills siblings ────────────────────────
// Fork a child that spawns spinning threads then execs echo.
// exec() must kill + join siblings before replacing the address space.
// If siblings were not killed, exec would corrupt their stacks → panic.

void test_exec(void)
{
    printf("test_exec: ");
    int pid = fork();
    if (pid == 0)
    {
        clone(spin_fn, 0);
        clone(spin_fn, 0);
        char *argv[] = {"echo", "exec_ok", 0};
        exec("echo", argv);
        printf("exec failed\n");
        exit(1);
    }
    int status;
    wait(&status);
    if (status != 0)
    {
        printf("FAILED (status=%d)\n", status);
        exit(1);
    }
    printf("OK\n");
}

// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    test_basic();
    test_exit();
    test_fork();
    test_exec();
    printf("all tests passed\n");
    exit(0);
}
