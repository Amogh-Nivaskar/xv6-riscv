#include "kernel/types.h"
#include "user/user.h"

// 40 page-aligned dead functions — each occupies a full 4096-byte page in
// the ELF text segment. Eager exec loads all 40 pages during loadseg.
// Lazy exec never loads them since main() never calls any of them.
#define DEAD(n) \
  __attribute__((noinline, aligned(4096))) \
  static void dead##n(void) { volatile int x = n; (void)x; }

DEAD(0)  DEAD(1)  DEAD(2)  DEAD(3)  DEAD(4)
DEAD(5)  DEAD(6)  DEAD(7)  DEAD(8)  DEAD(9)
DEAD(10) DEAD(11) DEAD(12) DEAD(13) DEAD(14)
DEAD(15) DEAD(16) DEAD(17) DEAD(18) DEAD(19)
DEAD(20) DEAD(21) DEAD(22) DEAD(23) DEAD(24)
DEAD(25) DEAD(26) DEAD(27) DEAD(28) DEAD(29)
DEAD(30) DEAD(31) DEAD(32) DEAD(33) DEAD(34)
DEAD(35) DEAD(36) DEAD(37) DEAD(38) DEAD(39)

// Reference all dead functions so the linker doesn't eliminate them.
// main() never calls this table.
void (*dead_table[])(void) = {
  dead0,  dead1,  dead2,  dead3,  dead4,
  dead5,  dead6,  dead7,  dead8,  dead9,
  dead10, dead11, dead12, dead13, dead14,
  dead15, dead16, dead17, dead18, dead19,
  dead20, dead21, dead22, dead23, dead24,
  dead25, dead26, dead27, dead28, dead29,
  dead30, dead31, dead32, dead33, dead34,
  dead35, dead36, dead37, dead38, dead39,
};

int main(void) {
  char ready = 'x';
  char buf[1];
  write(1, &ready, 1);  // stack variable avoids vmfault under pipe spinlock
  read(0, buf, 1);
  exit(0);
}
