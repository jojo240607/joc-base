#ifndef TASK_IDLE_H
#define TASK_IDLE_H

/* 最低优先级空闲任务：永远 READY，主动让出；保证就绪队列永不为空。 */
void idle_task(void *arg);

#endif /* TASK_IDLE_H */
