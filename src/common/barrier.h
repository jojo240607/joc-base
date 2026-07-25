#ifndef JOC_BASE_COMMON_BARRIER_H
#define JOC_BASE_COMMON_BARRIER_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * 内存屏障（barrier）
 *
 * 用途：保证无锁数据结构（spsc / ringbuffer / atomic）的“发布-订阅”顺序。
 *   例：上半部 ISR 先写数据缓冲区，再写 commit 标志；下半部任务必须先看到
 *   commit 标志，才能读数据——中间必须有 DMB 阻止 CPU/编译器乱序。
 *
 * 自包含：不依赖 cmsis，host 与 arm 都能编译。
 *   - ARM Cortex-M：使用硬件 DMB/DSB/ISB 指令；
 *   - 其它（host x86）：退化为编译器屏障（阻止编译器重排；x86 强内存模型
 *     本身不重排写，硬件语义由平台保证），足以让 host 单元测试正确。
 * ------------------------------------------------------------------------- */

/* 编译器屏障：阻止编译器跨此点移动内存访问（不生成任何指令） */
#define barrier_compiler()  __asm__ volatile("" ::: "memory")

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || \
    (defined(__CORTEX_M) && __CORTEX_M >= 3)
  /* 数据内存屏障：保证它之前的所有显式内存访问先于它之后的访问完成（对其它主设备可见） */
  #define barrier_dmb()  __asm__ volatile("dmb" ::: "memory")
  /* 数据同步屏障：保证它之前的内存访问全部完成才执行后续指令（比 dmb 更强） */
  #define barrier_dsb()  __asm__ volatile("dsb" ::: "memory")
  /* 指令同步屏障：冲刷流水线，保证之后的指令用新上下文（如 MPU/向量表改动后）取指 */
  #define barrier_isb()  __asm__ volatile("isb" ::: "memory")
#else
  /* host：硬件屏障无对应语义，用编译器屏障近似 */
  #define barrier_dmb()  __asm__ volatile("" ::: "memory")
  #define barrier_dsb()  __asm__ volatile("" ::: "memory")
  #define barrier_isb()  __asm__ volatile("" ::: "memory")
#endif

/* ---------------------------------------------------------------------------
 * 配套读写屏障（语义化别名，便于在无锁代码中清晰表达意图）
 * ------------------------------------------------------------------------- */
#define barrier_read()   barrier_dmb()   /* 读后屏障：后续读不会早于本屏障前的读 */
#define barrier_write()  barrier_dmb()   /* 写前屏障：前面的写不会晚于本屏障后的写 */

/* 把写入发布给其它上下文（ISR->任务 / 任务->ISR）的标准配对：
 *   写数据 ...
 *   barrier_publish();        // = dmb，确保数据先落，再写标志
 *   写“数据就绪”标志（atomic_store / 写 volatile）
 * 另一端读到标志后：
 *   读标志 ...
 *   barrier_acquire();        // = dmb，确保读 flag 之后才读数据
 *   读数据 ...
 */
#define barrier_publish()  barrier_dmb()
#define barrier_acquire()  barrier_dmb()

#endif /* JOC_BASE_COMMON_BARRIER_H */
