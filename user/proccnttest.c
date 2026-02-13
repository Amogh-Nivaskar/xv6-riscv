#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int count = getproccount();
  printf("The active process count = %d\n", count);
  exit(0);
}

