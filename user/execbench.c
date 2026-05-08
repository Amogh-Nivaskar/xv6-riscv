#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

// Measures the average cost of a full fork+exec+wait round-trip.
// With lazy ELF loading, exec() only records VMAs and returns without
// reading any ELF pages from disk, so this should be significantly
// cheaper than eager exec which reads the entire binary upfront.
void exec_latency(int n){
    printf("=== exec latency (N=%d) ===\n", n);

    uint64 total = 0;
    
    for (int i=0; i<n; i++){
        uint64 start = rdcycle();
        int pid = fork();

        if (pid == 0){
            char *args[] = {"fatbin", 0};
            exec("fatbin", args);
            exit(1);
        }else{
            wait(0);
            uint64 end = rdcycle();
            total += end - start;
        }
    }

    printf("Avg Exec Latency: %lu cycles\n", total / n);
}

// Measures physical memory consumed per live process immediately after exec.
// With lazy ELF loading, a process that has just exec'd but not yet touched
// its code pages holds very few physical pages. With eager exec, all ELF
// pages are mapped at exec time regardless of whether they are ever used,
// so each process consumes much more physical memory from the start.
void mem_footprint(int n){
    printf("=== memory footprint (N=%d) ===\n", n);

    int start_free = countfree();
    int release[2], ready[2];
    pipe(ready);
    pipe(release);
    
    for (int i=0; i<n; i++){
        int pid = fork();

        if (pid == 0){
            close(release[1]);
            close(0);
            dup(release[0]);
            close(release[0]);

            close(ready[0]);
            close(1);
            dup(ready[1]);
            close(ready[1]);

            char *args[] = {"fatnoopwait", 0};
            exec("fatnoopwait", args);
            exit(1);
        }
    }
    
    close(release[0]);
    close(ready[1]);
    char buf[1];
    for(int got = 0; got < n; got++)
        read(ready[0], buf, 1);
    int end_free = countfree();
    printf("Avg Pages Per Process: %d\n", (start_free-end_free)/n);


    for (int i = 0; i < n; i++)
        write(release[1], "x", 1);

    for (int i=0; i<n; i++)
        wait(0);
}

__attribute__((noinline)) void
dummy(void)
{
  volatile int x = 0;
  for (int i = 0; i < 100; i++)
    x += i;
}


// Measures the cost of calling the same function repeatedly.
// With lazy ELF loading, the first call triggers a page fault: the kernel
// must allocate a physical page, read the code from disk, and map it.
// Subsequent calls hit the already-mapped page and are cheap. This exposes
// the per-page fault penalty that lazy exec trades for its startup savings.
// With eager exec, all calls should cost roughly the same (no spike on call 1).
void jitter(void) {
    printf("=== jitter ===\n");

    for (int i = 0; i < 10; i++) {
        uint64 start = rdcycle();
        dummy();
        uint64 end = rdcycle();
        printf("call %d: %lu cycles\n", i+1, end - start);
    }
}


int
main(void)
{
    exec_latency(100);
    mem_footprint(10);
    jitter();
    exit(0);
}
