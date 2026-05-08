#include "kernel/types.h"
#include "user/user.h"

int main(void) {
  char buf[1];
  read(0, buf, 1);
  exit(0);
}
