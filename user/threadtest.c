#include "kernel/types.h"
#include "user/user.h"

volatile int done = 0;

void fn(void *arg)
{
    printf("%d\n", (int)(uint64)arg);
    done = 1;
    while (1)
        ;
}

int main(int argc, char *argv[])
{
    clone(fn, (void *)11);

    while (done != 1)
        ;

    printf("main done\n");

    exit(0);
}
