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
    printf("Running with SCHED_FIFO\n");

    for (int i = 0; i < 10; i++)
    {
        fork();
    }
    sleep(abs(rand() % 10));
    
    return 0;
}