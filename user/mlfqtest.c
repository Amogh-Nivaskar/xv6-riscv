#include "kernel/types.h"
#include "user/user.h"
#include "kernel/procinfo.h"

int getprocentry(int pid, struct procinfo* pi, struct procentry* pe){

    for (int i=0; i < pi->count; i++){
        struct procentry currentry = pi->entries[i];
        if (currentry.pid == pid){
            pe->pid = currentry.pid;
            pe->cpu_time = currentry.cpu_time;
            pe->state = currentry.state;
            pe->priority = currentry.priority;
            strcpy(pe->name, currentry.name);
            return 0;
        }
    }
    return -1;
}

int
main(int argc, char *argv[])
{
  printf("Start uptime = %d\n", uptime());
  int pid = getpid();

  struct procinfo pi;
  struct procentry myentry;
  int N = 30;
  int CYCLES = 10000000;

  for (int round = 0; round < N; round++){
    int ret = getprocinfo(&pi);
    if (ret < 0)
        printf("getprocinfo() FAILED!!");

    int r = getprocentry(pid, &pi, &myentry);

    if (r == 0)
        printf("R%d: Priority = %d | CPU Time = %d\n", round, myentry.priority, myentry.cpu_time);

    volatile int x = 0;
    for (int i=0; i < CYCLES; i++){
        x++;
    }
  }

  printf("End uptime = %d\n", uptime());
  exit(0);
};

