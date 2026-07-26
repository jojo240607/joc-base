#ifndef TASK_BIST_H
#define TASK_BIST_H

/* 板级自测后台任务：在独立的低优先级任务里跑自测，避免阻塞 main 任务的控制台。 */
void bist_task(void *arg);

#endif /* TASK_BIST_H */
