#ifndef JOC_RTOS_CONFIG_H
#define JOC_RTOS_CONFIG_H

/* ---------------------------------------------------------------------------
 * jOS RTOS 编译期配置（对标 FreeRTOSConfig.h）
 * 这些常量在编译期决定内核规模，全部为常量表达式，零运行时开销。
 * ------------------------------------------------------------------------- */

/* 优先级级数（0 = 最高，31 = 最低）。就绪队列用位图，选最高就绪任务 O(1)。 */
#ifndef RTOS_MAX_PRIORITIES
  #define RTOS_MAX_PRIORITIES 32
#endif

/* 系统节拍频率（Hz）。systick 1 kHz 提供 1 ms 调度粒度。 */
#ifndef RTOS_TICK_HZ
  #define RTOS_TICK_HZ 1000
#endif

/* 任务对象池容量（静态分配，无堆，确定性）。 */
#ifndef RTOS_MAX_TASKS
  #define RTOS_MAX_TASKS 8
#endif

/* 预定义任务优先级（数值越小优先级越高）。 */
#ifndef RTOS_PRIO_IDLE
  #define RTOS_PRIO_IDLE  31   /* 最低优先级：永远 READY 的空闲任务 */
#endif
#ifndef RTOS_PRIO_MAIN
  #define RTOS_PRIO_MAIN  16   /* 应用主任务（命令循环 + BIST） */
#endif
#ifndef RTOS_PRIO_BLINK
  #define RTOS_PRIO_BLINK 8    /* 演示用高优先级任务 */
#endif

/* 是否启用 MPU（P2 阶段置 1） */
#ifndef RTOS_USE_MPU
  #define RTOS_USE_MPU 0
#endif

#endif /* JOC_RTOS_CONFIG_H */
