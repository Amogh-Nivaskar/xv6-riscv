#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int *arr = (int *)sbrk(50000000);

    if (arr == (void *)-1){
        printf("sbrk failed\n");
    }

    arr[0] = 1;
    arr[1] = 2;

    int pid = fork();

    if (pid < 0) {
        printf("Fork has failed\n");

    } else if (pid == 0) {
        
        if (arr[0] != 1) {
            printf("FAILED: Child arr[0] expected 1, got %d\n", arr[0]);
            exit(1);
        }
        if (arr[1] != 2) {
            printf("FAILED: Child arr[1] expected 2, got %d\n", arr[1]);
            exit(1);
        }
        arr[0] = 3;
        arr[1] = 4;
        exit(0);
    } else {
        int status;
        wait(&status);

        if (status != 0) {
            printf("FAILED: child reported failure\n");
            exit(1);
        }

        if (arr[0] != 1) {
            printf("FAILED: Parent arr[0] expected 1, got %d\n", arr[0]);
            exit(1);
        }
        if (arr[1] != 2) {
            printf("FAILED: Parent arr[1] expected 2, got %d\n", arr[1]);
            exit(1);
        }

        printf("CoW Shared Pages Test: OK\n");

    }
    return 0;

}
