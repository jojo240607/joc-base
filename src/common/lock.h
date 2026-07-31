#ifndef JOC_BASE_COMMON_LOCK_H
#define JOC_BASE_COMMON_LOCK_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * 关中断 / 调度锁（lock）
 *
 * RTOS 内核的两层临界区原语（比现有 osal_enter_critical 更细分）：
 *   - irq_lock  / irq_unlock  ：操作 PRIMASK，关/开全局中断。用于极短、绝不允许
 *                               被任何 ISR 打断的区段（如就绪位图改写）。
 *   - sched_lock / sched_unlock：操作 BASEPRI，只屏蔽“优先级不高于阈值”的中断，
 *                               允许更高优先级的硬实时 ISR 继续响应，但阻止任务
 *                               切换（PendSV）。用于“需要原子、但不想关中断”的区段，
 *                               是高实时内核避免中断抖动的关键。
 *
 * 关键区别（务必理解）：
 *   irq_lock   关闭所有中断（包括最高优先级 ISR）——实时性最差，尽量少用、短用；
 *   sched_lock 仅挡住“会触发调度”的中断与 PendSV，最高优先级 ISR 仍可达——
 *               这就是 Zephyr/FreeRTOS 的“调度锁”思路，本 RTOS 沿用。
 *
 * 自包含：ARM 用内联汇编直接操作 PRIMASK/BASEPRI；host 用空操作桩（仅验证
 *         接口与逻辑，真实硬件语义由后续板载 BIST 覆盖）。
 * ------------------------------------------------------------------------- */

/* 关中断状态：保存 PRIMASK（bit0=1 表示已关中断）*/
typedef uint32_t irq_state_t;

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || \
    (defined(__CORTEX_M) && __CORTEX_M >= 3)

static inline irq_state_t irq_lock(void) {
    uint32_t state;
    __asm__ volatile("mrs %0, PRIMASK" : "=r"(state));
    __asm__ volatile("cpsid i" ::: "memory");   /* 关全局中断 */
    return state;
}

static inline void irq_unlock(irq_state_t state) {
    if ((state & 0x1U) == 0U) {                 /* 仅当原本是开中断时才恢复 */
        __asm__ volatile("cpsie i" ::: "memory");
    }
}

/* NVIC 优先级位宽（M3/M4/M7 多为 4 位；device header 通常会定义，这里给默认值）*/
#ifndef __NVIC_PRIO_BITS
  #define __NVIC_PRIO_BITS 4U
#endif

/* 调度锁：把 BASEPRI 设为 prio（逻辑优先级 0..(1<<__NVIC_PRIO_BITS)-1），
 *         屏蔽“数值上不高于 prio 的所有异常”（含 PendSV/SVC），但更高优先级
 *         的 ISR 仍可抢占。prio==0 等价于不屏蔽任何（即 sched_unlock）。*/
static inline void sched_lock(uint8_t prio) {
    uint32_t val = ((uint32_t)prio) << (8U - __NVIC_PRIO_BITS);
    __asm__ volatile("msr BASEPRI, %0" :: "r"(val) : "memory");
}

/* 解调度锁：BASEPRI 写 0 = 取消屏蔽，恢复正常可抢占 */
static inline void sched_unlock(void) {
    __asm__ volatile("msr BASEPRI, %0" :: "r"(0UL) : "memory");
}

/* 当前是否处于关中断状态（供断言用：误在关中断时调可能死锁的路径） */
static inline bool irq_is_disabled(void) {
    uint32_t state;
    __asm__ volatile("mrs %0, PRIMASK" : "=r"(state));
    return (state & 0x1U) != 0U;
}

/* 是否处于中断上下文（Handler 模式）：用 MRS 读 IPSR（异常号）判断。
 * 早期实现直接读内存映射的 SCB->ICSR(0xE000ED04)，但该寄存器在非特权态不可读，
 * 会导致“非特权任务调用 rtos_need_svc()→arch_in_isr()”时触发 BusFault（见 Task#1
 * 自测：USR 任务一进 SVC 判定就崩）。IPSR 是 xPSR 的一部分，任意特权级都可 MRS 读取，
 * 且 IPSR[8:0] 与 ICSR.VECTACTIVE 等价（线程态为 0，异常态为异常号），故行为一致。 */
static inline int arch_in_isr(void) {
    uint32_t ipsr;
    __asm__ volatile("mrs %0, IPSR" : "=r"(ipsr));
    return ((ipsr & 0x1FFu) != 0u);
}

/* 是否处于特权模式（CONTROL.nPRIV==0）。用于“某些寄存器（如 DWT 调试部件）非特权
 * 不可访问”的路径：非特权态下读 DWT->CYCCNT 会触发 BusFault，故临界区审计只在特权
 * 上下文读取周期计数器（非特权任务的临界区均经 SVC 在 Handler 模式执行，已是特权）。 */
static inline int arch_in_priv(void) {
    uint32_t ctrl;
    __asm__ volatile("mrs %0, CONTROL" : "=r"(ctrl));
    return ((ctrl & 0x1u) == 0u);   /* nPRIV=0 => 特权 */
}

#else  /* host / 非 arm：空操作桩，仅用于编译与逻辑自测 */

static inline irq_state_t irq_lock(void)        { return 0U; }
static inline void irq_unlock(irq_state_t s)    { (void)s; }
static inline void sched_lock(uint8_t prio)     { (void)prio; }
static inline void sched_unlock(void)           { }
static inline bool irq_is_disabled(void)        { return false; }

/* host 桩：永不处于中断上下文 */
static inline int arch_in_isr(void)              { return 0; }

/* host 桩：host 始终视为特权 */
static inline int arch_in_priv(void)             { return 1; }

#endif

/* ---------------------------------------------------------------------------
 * 便捷封装：作用域临界区（C 无 RAII，用一对调用模拟；务必配对）
 * ------------------------------------------------------------------------- */
#define LOCK_IRQ_SCOPE() \
    for (irq_state_t _ls = (irq_lock(), 0U); !_ls; _ls = (irq_unlock(_ls), 1U))

#endif /* JOC_BASE_COMMON_LOCK_H */
