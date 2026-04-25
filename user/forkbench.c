#include "kernel/types.h"
#include "user/user.h"

#define MEM_PAGES 4096
#define PGSIZE    4096
#define TRIALS    500

int main() {
    
    char *buf = (char *)sbrk(MEM_PAGES * PGSIZE);
    uint64 start, end;

    for (int i = 0; i < MEM_PAGES; i++)
        buf[i * PGSIZE] = i;       

    start = rdcycle();
    for (int i=0; i<TRIALS; i++){
        int pid = fork();
        if (pid == 0) {
            exit(0);                   
        } else {
            wait(0);
        }
    }
    end = rdcycle();

    printf("No-Write Fork with %d pages: %ld cycles\n", MEM_PAGES, (end - start)/TRIALS);

    start = rdcycle();
    for (int i=0; i<TRIALS; i++){
        int pid = fork();
        if (pid == 0) {
            for (int j=0; j < MEM_PAGES; j++){
                buf[j * PGSIZE] = j;
            }
            exit(0);                   
        } else {
            wait(0);
        }
    }
    end = rdcycle();

    printf("Write-All Fork with %d pages: %ld cycles\n", MEM_PAGES, (end - start)/TRIALS);

    start = rdcycle();
    for (int i=0; i<TRIALS; i++){
        int pid = fork();
        if (pid == 0) {
            for (int j=0; j < MEM_PAGES; j += 4){
                buf[j * PGSIZE] = j;
            }
            exit(0);                   
        } else {
            wait(0);
        }
    }
    end = rdcycle();

    printf("Write 1/4th Fork with %d pages: %ld cycles\n", MEM_PAGES, (end - start)/TRIALS);

    start = rdcycle();
    for (int i=0; i<TRIALS; i++){
        int pid = fork();
        if (pid == 0) {
            char *args[] = {"noop", 0};
            exec("noop", args);
            exit(0);                   
        } else {
            wait(0);
        }
    }
    end = rdcycle();

    printf("Fork + Exec with %d pages: %ld cycles\n", MEM_PAGES, (end - start)/TRIALS);


    uint64 pages = 256;
    while(1) {
        char *mem = (char *)sbrk(pages * PGSIZE);
        if (mem == (char*)-1) {
            printf("Max forkable: %ld pages (sbrk failed)\n", MEM_PAGES + pages);
            break;
        }
        int pid = fork();
        if (pid < 0) {
            printf("Max forkable: %ld pages\n", MEM_PAGES + pages);
            break;
        } else if (pid == 0) {
            exit(0);
        } else {
            wait(0);
            pages *= 2;
        }
    }

    exit(0);
}