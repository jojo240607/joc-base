#ifndef JOC_RTOS_ARCH_CORTEX_M_ARCH_LOCK_H
#define JOC_RTOS_ARCH_CORTEX_M_ARCH_LOCK_H

/* ===========================================================================
 * arch/cortex_m 关中断 / 调度锁原语（ARMv7-M）
 *
 * 这是与 Cortex-M ISA 强绑定的临界区原语，从 common/lock.h 归位到 arch 层。
 * common/lock.h 退化为薄 shim，按编译器宏分派到本头（Cortex-M）或
 * arch/riscv/arch_lock.h（RISC-V）；host 单元测试用空操作桩（留在 lock.h）。
 *
 * 同 ISA 换芯片（STM32F4 -> STM32F7）只调 cortex_m.h 的移植旋钮，本头不动；
 * 换 ISA 只需新增 arch/<isa>/arch_lock.h 并在 lock.h 加一个分派分支。
 * ========================================================================= */

#include <stdint.h>
#include <stdbool.h>

/* NVIC 优先级位宽（M3/M4/M7 多为 4 位；device header 通常会定义，这里给默认值）*/
#ifndef __NVIC_PRIO_BITS
  #define __NVIC_PRIO_BITS 4U
#endif

/* 关全局中断：读 PRIMASK 保存旧态，再 cpsid i 关中断。返回 bit0=1 表示原本已关。 */
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

#endif /* JOC_RTOS_ARCH_CORTEX_M_ARCH_LOCK_H */
