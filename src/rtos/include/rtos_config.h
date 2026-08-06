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
  #define RTOS_MAX_TASKS 48   /* 留余量：常驻任务(~13) + RTOSALL 长串联(P4 创建~18 + 各 TC 动态任务) 仍安全；CCM TCB 池开销小 */
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
#ifndef RTOS_PRIO_RTOSALL_RUNNER
  /* RTOSALL 运行器优先级：bist 任务默认 prio 24，而各子测会创建优先级【高于】它的
   * 任务(rtos_basic T03=18 / T04=6 / T05=5、rtos_irq irq_a=3 等)。若以 bist 原优先级
   * 运行 rtos_selftest_run_all，这些更高优先级子任务会永久抢占 bist，使其被 msleep
   * 唤醒后无法设置 stop 标志，造成饿死/死锁(实测卡在 basic/T03)。故运行前把 bist 的
   * 有效优先级临时顶到本值(>所有子测任务的最高优先级 3)，跑完再恢复。
   * 取 2：高于子测任务最高优先级(3)，且为普通任务优先级(非 ISR，不受零延迟阈值 4 约束)；
   * 仅在 RTOSALL 一次性运行期间短暂提升，不影响常态调度。 */
  #define RTOS_PRIO_RTOSALL_RUNNER 2
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

/* 软件定时器（docs/rtos-test-plan.md §6.3，准则 rtos-test.md §2.5）：单次/周期定时器，
 * 回调在专用“定时器任务”上下文执行；过期判定用无符号 tick 比较，故 32 位 g_tick 在
 * 48 天(0xFFFFFFFF→0)翻转后仍正确。定时器任务在 rtos_start 里一次性创建（与 workqueue
 * 同款约束：绝不从 ISR 懒建任务），默认随 RTOS 编译进内核。 */
#ifndef RTOS_USE_TIMERS
  #define RTOS_USE_TIMERS 1
#endif
#ifndef RTOS_PRIO_TIMER
  #define RTOS_PRIO_TIMER 5   /* 定时器任务优先级：高于应用(main 16)、低于零延迟 ISR */
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

/* P2-1 调度轨迹 trace：在唯一切换点（rtos_pendsv_switch）记录 (tick, from, to, reason)
 * 到固定环形缓冲，供 RTOSTRACE 命令导出。默认关闭（零开销、零 RAM）；开发/诊断构建
 * 用 -DRTOS_SCHED_TRACE=1 开启（环形缓冲占 RTOS_TRACE_BUF_LEN*12B，默认 256*12≈3KB）。 */
#ifndef RTOS_SCHED_TRACE
  #define RTOS_SCHED_TRACE 0
#endif

/* 硬实时临界区持锁上限（见 docs/rtos-hard-realtime-plan.md 阶段2）：任何内核临界区
 * （rtos_crit_enter/exit、sched_lock 区间）的持有时长超过此 tick 数，即判定为
 * “长临界区阻塞高优任务”，递增粘性计数 g_rtos_crit_overflow（不触发异常、不停机）。
 * 默认 2ms@1kHz：高于典型 PendSV 切换(<30us) 两个数量级，仅抓真正危险的过长持锁。
 * 设 0 关闭审计（编译期）。
 *
 * 实现注意：锁调度/临界区用 BASEPRI 屏蔽了 SysTick（systick 优先级 15 落在被屏蔽带），
 * 故“持锁期间经过的 tick 数”概念不成立；审计改用 DWT CYCCNT（rtos_cycle_now，零延迟
 * ISR 仍计数、不受 BASEPRI 影响）测量真实持锁周期数，再换算成 tick 等价阈值。
 * 阈值周期 = RTOS_CRIT_MAX_TICKS * (RTOS_CPU_HZ / RTOS_TICK_HZ)。 */
#ifndef RTOS_CPU_HZ
  #define RTOS_CPU_HZ 168000000UL   /* 本项目 STM32F407 HCLK = 168 MHz */
#endif
#ifndef RTOS_CRIT_MAX_TICKS
  #define RTOS_CRIT_MAX_TICKS 2
#endif
#ifndef RTOS_CRIT_MAX_CYCLES
  /* 阈值周期 = RTOS_CRIT_MAX_TICKS * (RTOS_CPU_HZ / RTOS_TICK_HZ)
   * = 2 * (168000000 / 1000) = 336000 cycles（< uint32 上限，故直接整数常量）。
   * 若调整 RTOS_CRIT_MAX_TICKS / RTOS_CPU_HZ，请同步重算此值。 */
  #define RTOS_CRIT_MAX_CYCLES 336000U
#endif

/* 临界区硬上限执行（P0-3，见 docs/rtos-hard-realtime-roadmap.md）：把“报告超长持锁”
 * 升级为“可配置故障处理”。默认 0 = 仅报告（不阻塞、不停机，与 P3 行为完全一致，
 * 零回归）。可设值：
 *   0 = RTOS_CRIT_KILL_REPORT  仅递增 g_rtos_crit_overflow（粘性，零挂起风险）
 *   1 = RTOS_CRIT_KILL_TASK    杀掉持该超长临界区的任务（rtos_task_delete(g_running)）
 *   2 = RTOS_CRIT_KILL_WDT     武装独立看门狗（IWDG，2s 超时），违约升级为确定性复位
 *   3 = RTOS_CRIT_KILL_PANIC   进入安全态：置 g_rtos_crit_kill_panic 粘性标志并自旋
 *                               （非特权任务经 SVC 在 Handler 模式执行，g_running 有效）
 * 设置 !=0 时，rtos_crit_exit_audit 在检测到超长临界区退出最外层时按本值升级处理。
 * 注意：杀任务/看门狗联动/panic 都【不】引入新挂起路径——审计只在退出最外层临界区时
 * 发生，此时调度锁已即将放开，kill/wdt/panic 在特权态安全执行。 */
#ifndef RTOS_CRIT_KILL
  #define RTOS_CRIT_KILL 0
#endif
#define RTOS_CRIT_KILL_REPORT 0
#define RTOS_CRIT_KILL_TASK   1
#define RTOS_CRIT_KILL_WDT    2
#define RTOS_CRIT_KILL_PANIC  3

/* 硬实时看门狗联动（见 docs/rtos-hard-realtime-plan.md 阶段4 §4.3）：
 * 开启后，若任一硬性实时违约计数（deadline_miss / wcet_miss / crit_overflow /
 * sched_invalid）非零，内核在 tick 中武装独立看门狗（IWDG），使违约升级为确定性
 * 复位/报警，而非静默漏过。默认关闭：避免自测故意触发的违约（deadline 自测反例）
 * 导致开发板意外复位；生产构建可开启获得确定性故障处理。
 * 注意：deadline/wcet/crit 自测在返回前已恢复全局聚合计数，故开启本宏时运行
 * RTOSALL 不会因自测反例误复位（任务级 miss 仍留，但全局聚合已清零）。 */
#ifndef RTOS_HARD_RT_WDT
  #define RTOS_HARD_RT_WDT 0
#endif

/* 零延迟 IRQ / BASEPRI 阈值（见 docs/rtos-design.md §4.5）：高于该“优先级数”的极少
 * 数最高优先级 ISR 永不被内核临界区屏蔽（用 BASEPRI 而非 PRIMASK 关中断）。
 * 本项目默认 4 = 启用选择性屏蔽：配合语义带 IRQ_PRIO_KERNEL=5 / IRQ_PRIO_ZERO_LATENCY=2
 * 满足 FreeRTOS 式契约——内核 ISR 优先级 5 >= 4 被 BASEPRI 屏蔽，零延迟 ISR 优先级
 * 2 < 4 永不被屏蔽；rtos_start 会把 SysTick 置于可被屏蔽带、PendSV 恒为最低。
 * 设为 0 则退化为全局关中断( PRIMASK )，行为与此前完全一致。
 * 启用(>0)时须遵守契约：所有调用内核 API 的 ISR 优先级必须 >= 此值，否则
 * rtos_start 的 irq_manager_audit_priorities 会打印违例。 */
#ifndef RTOS_MAX_ZERO_LATENCY_IRQS
  #define RTOS_MAX_ZERO_LATENCY_IRQS 4
#endif

/* ---------------------------------------------------------------------------
 * 是否包含 RTOS 自测（RTOS_SELFTEST_ADD 段收集 + 各 rtos_*_selftest + RTOS* 控制台
 * 命令 + 板载 BIST）。
 *
 *  - 开发 / 覆盖率构建默认开启（见 CMake 的 RTOS_SELFTEST 选项，经 -D 控制）。
 *  - 发布构建关闭：剔除全部自测代码与攻击面；公开头 rtos.h 不再暴露 *_selftest()
 *    声明；链接段 .rtos_selftests 为空，rtos_selftest_run_all 不被编译。
 *  - 关闭后 RTOS_SELFTEST_ADD 退化为 no-op 宏（见 rtos.h），故即使在混合文件中
 *    残留注册调用也不会产生段内容。
 * 此处仅提供默认值；CMake 通过 -D 覆盖（#ifndef 保证两者一致）。
 * ------------------------------------------------------------------------- */
#ifndef RTOS_SELFTEST
  #define RTOS_SELFTEST 1
#endif

/* 配套的语义化优先级带（定义在 src/irq/irq.h，驱动经 irq_manager_set_priority 使用，
 * 取代散落的魔法数字）：
 *   IRQ_PRIO_KERNEL=5       调用内核 API 的 ISR（必须 >= 本阈值，才会被 BASEPRI 屏蔽）
 *   IRQ_PRIO_ZERO_LATENCY=2 零延迟 ISR（必须 < 本阈值，永不被内核临界区屏蔽）
 *   IRQ_PRIO_DEFAULT=8      普通 ISR（不调内核 API、非抖动敏感）
 * 本项目即默认阈值 4：KERNEL(5) >= 4 被 BASEPRI 屏蔽、ZERO_LATENCY(2)
 * < 4 永不被屏蔽，两者都满足上面的契约；rtos_start 的 irq_manager_audit_priorities
 * 会在违例时打印错误。 */

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
