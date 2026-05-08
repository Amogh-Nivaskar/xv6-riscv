#include "kernel/types.h"
#include "user/user.h"

// Large initialized data segment — forces the ELF file to contain 100 pages
// of data that eager exec must read from disk. Lazy exec never reads them
// since the program exits without touching this array.
char bigdata[50 * 4096] = {1};

int main(void) {
  exit(0);
}
