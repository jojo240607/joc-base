#include "task_idle.h"
#include "rtos.h"

RTOS_TASK_STACK(g_idle_stack, 512);

void idle_task(void *arg)
{
    (void)arg;
    for (;;) rtos_yield();
}

RTOS_TASK(idle, "idle", idle_task, RTOS_PRIO_IDLE, g_idle_stack, sizeof(g_idle_stack), (void *)0, 1);
