#include "kernel/types.h"
#include "user/user.h"

int 
main(int argc, char* argv[]){

    int ticks = 10;
    
    int before = uptime();
    printf("Sleeping for %d ticks...\n", ticks);
    sleep(ticks);
    int after = uptime();
    printf("Slept for %d ticks\n", after - before);

    exit(0);

}