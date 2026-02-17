#include "kernel/types.h"
#include "kernel/procinfo.h"
#include "user/user.h"

int 
main(int argc, char* argv[]){
    struct procinfo pi;
    int ret = getprocinfo(&pi);

    if (ret != 0){
        printf("Error in getprocinfo() syscall !!");
    }

    printf("Process Count: %d\n", pi.count);

    for (int i = 0; i < pi.count; i++){
        printf("%s\n", pi.names[i]);
    }

    exit(0);

}