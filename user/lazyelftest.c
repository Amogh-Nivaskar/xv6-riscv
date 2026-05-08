#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

#define PGSIZE 4096

int main(void);


// -----------------------------------------------------------------------
// Test 1: The laziness property — free pages decrease as code executes.
//
// free_start is measured at the top of main() before most code/data pages
// of this binary have been touched.  free_end is measured after all other
// tests have run and loaded their pages.  With lazy ELF loading, many
// pages are demand-loaded between those two points, so free_end < free_start.
// With eager exec all pages load at exec time and both counts are equal.
//
// NOTE: free_start is captured in main() and passed in here so that the
// measurement happens as early as possible, before any other function body
// is entered.
// -----------------------------------------------------------------------
static int free_start;

void
test_lazy_property(void)
{
  int free_end = countfree();
  int loaded = free_start - free_end;
  if (loaded <= 0) {
    printf("FAILED test_lazy_property: free pages did not decrease "
           "(%d -> %d); ELF pages may have been loaded eagerly\n",
           free_start, free_end);
    exit(1);
  }
  printf("test_lazy_property: %d page(s) demand-loaded during execution: OK\n",
         loaded);
}

// -----------------------------------------------------------------------
// Test 2: BSS pages are zero-initialized on demand.
//
// The static array lives in the BSS segment and is never touched before
// this test runs, so no physical page has been mapped for it yet.
// vmfault should zero-fill each page the first time it is accessed.
// -----------------------------------------------------------------------
static char bss[4 * PGSIZE];

void
test_bss(void)
{
  for (int i = 0; i < (int)sizeof(bss); i++) {
    if (bss[i] != 0) {
      printf("FAILED test_bss: bss[%d] = %d, want 0\n", i, bss[i]);
      exit(1);
    }
  }
  printf("test_bss: OK\n");
}

// -----------------------------------------------------------------------
// Test 3: Pages loaded by forked children are freed when they exit.
//
// countfree() is measured around a batch of fork/wait pairs.  Between
// the two measurements this process calls only already-loaded functions
// (fork, wait, countfree), so any drop is a genuine page leak.
// -----------------------------------------------------------------------
void
test_child_pages_freed(void)
{
  int free0 = countfree();

  for (int i = 0; i < 5; i++) {
    int pid = fork();
    if (pid < 0) { printf("FAILED test_child_pages_freed: fork\n"); exit(1); }
    if (pid == 0) exit(0);
    int st;
    wait(&st);
    if (st != 0) { printf("FAILED test_child_pages_freed: child exit %d\n", st); exit(1); }
  }

  if (countfree() < free0) {
    printf("FAILED test_child_pages_freed: pages leaked after children exited\n");
    exit(1);
  }
  printf("test_child_pages_freed: OK\n");
}

// -----------------------------------------------------------------------
// Test 4: Fork inherits VMAs; child can independently demand-load pages.
//
// Parent initialises a static variable, forks, child overwrites it.
// After the child exits the parent's copy must be unchanged — verifies
// that CoW + lazy VMA page semantics interact correctly.
// -----------------------------------------------------------------------
void
test_fork_vma(void)
{
  static int shared = 42;

  int pid = fork();
  if (pid < 0) { printf("FAILED test_fork_vma: fork\n"); exit(1); }
  if (pid == 0) {
    shared = 99;
    exit(0);
  }
  int st;
  wait(&st);
  if (st != 0) { printf("FAILED test_fork_vma: child exit %d\n", st); exit(1); }
  if (shared != 42) {
    printf("FAILED test_fork_vma: parent saw %d after child wrote, want 42\n", shared);
    exit(1);
  }
  printf("test_fork_vma: OK\n");
}

// -----------------------------------------------------------------------
// Test 5: Text segment pages are mapped read-only (no PTE_W).
//
// vmfault sets permissions from flags2perm(vma.flags): text pages carry
// PTE_R|PTE_X but not PTE_W.  A store to a code address must kill the
// process.  The parent forks a child that attempts the illegal write and
// verifies the child is killed (non-zero exit status).
// -----------------------------------------------------------------------
void
test_text_readonly(void)
{
  int pid = fork();
  if (pid < 0) { printf("FAILED test_text_readonly: fork\n"); exit(1); }
  if (pid == 0) {
    // Attempt to write to the code page containing main().
    // This should trigger a store fault and kill the child.
    *(volatile char *)main = 0;
    // If we reach here vmfault did not enforce read-only — fail.
    exit(0);
  }
  int st;
  wait(&st);
  if (st == 0) {
    printf("FAILED test_text_readonly: write to text page was not caught\n");
    exit(1);
  }
  printf("test_text_readonly: OK\n");
}

int
main(void)
{
  // Capture free page count as early as possible, before most code/data
  // pages of this binary have been demand-loaded.
  free_start = countfree();

  test_bss();
  test_child_pages_freed();
  test_fork_vma();
  test_text_readonly();

  // Must be last: compares free_start (captured above) against current count.
  test_lazy_property();

  printf("lazyelftest: all tests passed\n");
  exit(0);
}
