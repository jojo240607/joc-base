#ifndef JOC_BASE_COMMON_LOCK_H
#define JOC_BASE_COMMON_LOCK_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * 关中断 / 调度锁（lock）—— 薄 shim 分派层
 *
 * RTOS 内核的两层临界区原语（比现有 osal_enter_critical 更细分）：
 *   - irq_lock  / irq_unlock  ：操作 PRIMASK(ARM)/MIE(RISC-V)，关/开全局中断。
 *                               用于极短、绝不允许被任何 ISR 打断的区段（如就绪位图改写）。
 *   - sched_lock / sched_unlock：ARM 操作 BASEPRI 只屏蔽“优先级不高于阈值”的中断，
 *                               允许更高优先级的硬实时 ISR 继续响应，但阻止任务切换
 *                               （PendSV）；RISC-V 操作 mie.MSIE（阻止 msip 触发切换）。
 *                               用于“需要原子、但不想关中断”的区段，是高实时内核
 *                               避免中断抖动的关键。
 *
 * 关键区别（务必理解）：
 *   irq_lock   关闭所有中断（包括最高优先级 ISR）——实时性最差，尽量少用、短用；
 *   sched_lock 仅挡住“会触发调度”的中断与 PendSV，最高优先级 ISR 仍可达——
 *               这就是 Zephyr/FreeRTOS 的“调度锁”思路，本 RTOS 沿用。
 *
 * 分派策略：与 ISA 强绑定的实现已归位到 arch 层，本头只负责按编译器宏分派：
 *   - Cortex-M（__ARM_ARCH_7M__/__CORTEX_M>=3）→ arch/cortex_m/arch_lock.h
 *     （PRIMASK/BASEPRI/IPSR/CONTROL，经 ARCH_DIR include 路径解析）
 *   - RISC-V（__riscv）→ arch/riscv/arch_lock.h
 *     （mstatus.MIE/mie.MSIE/软件特权判定，同样经 ARCH_DIR include 路径解析）
 *   - host / 其它 → 本文件内空操作桩（仅验证接口与逻辑，PC 单元测试用）
 *
 * 同 ISA 换芯片（STM32F4 -> STM32F7 / ESP32C3 -> 其他 RV32）无需改本文件；
 * 换 ISA 只需新增 arch/<isa>/arch_lock.h 并在下方加一个分派分支。
 * ------------------------------------------------------------------------- */

/* 关中断状态：保存 PRIMASK（ARM, bit0=1 表示已关）/ MIE 位（RISC-V）*/
typedef uint32_t irq_state_t;

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || \
    (defined(__CORTEX_M) && __CORTEX_M >= 3)
  /* Cortex-M 原语见 arch/cortex_m/arch_lock.h（ARCH_DIR include 路径解析） */
  #include "arch_lock.h"
#elif defined(__riscv)
  /* RISC-V 原语见 arch/riscv/arch_lock.h（ARCH_DIR include 路径解析） */
  #include "arch_lock.h"
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
