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
  #define RTOS_MAX_TASKS 16
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
#ifndef RTOS_PRIO_BIST
  #define RTOS_PRIO_BIST 24    /* 板级自测：低优先级后台任务(不阻塞控制台) */
#endif

/* 下半部优先级带（见 docs/rtos-design.md 第 4 章）：高于应用任务、低于 ISR。
 * 数值越小优先级越高；BH_HIGH/BH_MED 均高于 RTOS_PRIO_MAIN(16)/BLINK(8)，
 * 保证“上半部触发的下半部”能尽快抢占应用任务执行耗时逻辑，同时永远被硬件
 * ISR 抢占。 */
#ifndef RTOS_PRIO_BH_HIGH
  #define RTOS_PRIO_BH_HIGH 4   /* 下半部高优先级带：实时关键路径(USB CDC/ CAN/ADC) */
#endif
#ifndef RTOS_PRIO_BH_MED
  #define RTOS_PRIO_BH_MED  6   /* 下半部中优先级带：非关键延迟工作(共享 workqueue) */
#endif

/* 是否启用 MPU（固定区域 + 栈哨兵 + MemManage 恢复；任务保持特权故对现行任务透明）。
 * 默认开启：P2 阶段已验证 MPU 自测（越权捕获/恢复）与 BIST 共存无回归。 */
#ifndef RTOS_USE_MPU
  #define RTOS_USE_MPU 1
#endif

/* 同优先级时间片轮转（Round-Robin，见 docs/rtos-design.md §1/§3）。
 * 开启后，同一优先级的多个就绪任务按 RTOS_TIME_SLICE_TICKS 个节拍轮流运行，
 * 避免某个计算密集型任务饿死同优先级兄弟任务。跨优先级仍是纯抢占式。 */
#ifndef RTOS_TIME_SLICE
  #define RTOS_TIME_SLICE 1
#endif
#ifndef RTOS_TIME_SLICE_TICKS
  #define RTOS_TIME_SLICE_TICKS 5   /* 每个任务连续运行 5 个节拍(5ms @1kHz)后让出 */
#endif

/* 零延迟 IRQ（见 docs/rtos-design.md §4.5）：高于该“优先级数”的极少数最高优先级
 * ISR 永不被内核临界区屏蔽（用 BASEPRI 而非 PRIMASK 关中断）。默认 0 = 关闭，
 * 内核临界区退化为全局关中断( PRIMASK )，行为与此前完全一致、零回归。
 * 启用(>0)时须遵守 FreeRTOS 式契约：所有调用内核 API 的 ISR 优先级必须 >= 此值
 *（否则零延迟 ISR 可能抢占总被 BASEPRI 屏蔽的临界区造成重入）；rtos_start 会把
 * SysTick/PendSV 置于可被屏蔽的优先级带。 */
#ifndef RTOS_MAX_ZERO_LATENCY_IRQS
  #define RTOS_MAX_ZERO_LATENCY_IRQS 0
#endif

/* ---------------------------------------------------------------------------
 * MPU SRAM 隔离（见 docs/rtos-design.md §6：R2 内核 RAM 仅特权 / R3 每任务栈 region）
 *
 * 8-region MPU 的硬约束：每任务无法独占多个 region（最多 16 任务、仅余 R4–R7）。
 * 因此采用“固定区域 + 每任务重编程 1 个栈 region(R4)”的折中，具体两项：
 *
 *  (R3) 每任务栈 region：默认开启。上下文切换时把 region R4 重编程为“当前任务栈”
 *       范围（unpriv RW、不可执行），并把最低 1/8 subregion 禁访 → 作为【栈底溢出
 *       哨兵】，非特权任务向下溢出即 MemManage Fault（特权任务由软件哨兵兜底）。
 *       要求任务栈为 2 的幂大小且基址对齐到该大小，请用 RTOS_TASK_STACK() 声明，
 *       否则自动退回软件哨兵（不报错、不误 fault）。
 *
 *  (R2) 内核 RAM 仅特权：默认关闭。开启后整块 SRAM 设为“仅特权 RW”，非特权任务
 *       只能访问自己的栈 region(R4) 与 SVC 门，无法直接读写内核 .data/.bss/堆/其它
 *       任务栈——实现真正的“内核/用户态 RAM 隔离”。⚠ 注意：当前系统“常态任务保持
 *       特权”、且非特权任务仍与内核共享 IPC 全局对象，开启此项会让非特权任务碰这些
 *       共享全局即 fault。它是为“纯用户态任务(只经 SVC 门访问内核)”场景准备的开关，
 *       默认关闭以保证与现行共享模型零回归；其机制由 rtos_mpu_selftest 的隔离子测试
 *       临时开启并验证（写内核全局 → MemFault → 恢复）。
 * ------------------------------------------------------------------------- */
#ifndef RTOS_MPU_PER_TASK_STACK
  #define RTOS_MPU_PER_TASK_STACK 1
#endif
#ifndef RTOS_MPU_PROTECT_KERNEL_RAM
  #define RTOS_MPU_PROTECT_KERNEL_RAM 0
#endif
#ifndef RTOS_MPU_STACK_REGION
  #define RTOS_MPU_STACK_REGION 4   /* 每任务栈 region 编号（R0-R3 已用于固定区） */
#endif

#endif /* JOC_RTOS_CONFIG_H */
