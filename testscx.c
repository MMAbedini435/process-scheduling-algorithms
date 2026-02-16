#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    struct sched_param param;
    param.sched_priority = 0; // Priority 1-99
    if (sched_setscheduler(0, 7, &param) == -1) {
        perror("sched_setscheduler");
        exit(EXIT_FAILURE);
    }
    printf("Running with SCHED_EXT\n");

    for (int i = 0; i < 2; i++)
    {
        fork();
    }
    sleep(abs(rand() % 10));
    int dostuff = 1;
    for (int i = 0; i < 50000; i++)
    {
        dostuff *= i;
    }
    sleep(abs(rand() % 10));
    for (int i = 0; i < 100000; i++)
    {
        dostuff *= i;
    }
    
    sleep(abs(rand() % 10));
    
    return 0;
}