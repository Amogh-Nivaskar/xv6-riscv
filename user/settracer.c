#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    if (argc != 2){
        printf("Invalid Argument: 1 (Enabled) or 0 (Disabled)");
        exit(1);
    }
    int value = atoi(argv[1]);
    if (value != 1 && value != 0){
        printf("Invalid Argument: 1 (Enabled) or 0 (Disabled)");
        exit(1);
    }

    int res = settracer(value);

    if (res < 0){
        printf("Error in setting tracer !!");
        exit(0);
    }

    if (value == 1){
        printf("Tracing Enabled\n");
    }else{
        printf("Tracing Disabled\n");
    }

    exit(0);
}