#ifndef JOC_RTOS_ARCH_RISCV_ARCH_LOCK_H
#define JOC_RTOS_ARCH_RISCV_ARCH_LOCK_H

/* ===========================================================================
 * arch/riscv 关中断 / 调度锁原语（RV32IMC）
 *
 * 这是与 RISC-V ISA 强绑定的临界区原语，从 common/lock.h 归位到 arch 层。
 * common/lock.h 退化为薄 shim，按编译器宏分派到本头（RISC-V）或
 * arch/cortex_m/arch_lock.h（Cortex-M）；host 单元测试用空操作桩（留在 lock.h）。
 *
 * RV32I 无 BASEPRI 对等物，关键实现说明：
 *   - irq_lock/irq_unlock：操作 mstatus.MIE（PRIMASK 对等物）。
 *   - sched_lock/unlock：屏蔽/恢复 mie.MSIE（机器软件中断位）。RTOS 的上下文
 *     切换由 msip 软件中断触发（PendSV 对等物，见 context.S .Ltrap_switch），
 *     屏蔽 MSIE 后 msip 挂起但不触发，即“阻止任务切换但允许更高优先级 ISR
 *     （定时器/外部中断）继续响应”——与 ARM BASEPRI 屏蔽 PendSV 语义一致。
 *     解锁恢复 MSIE 后，挂起的软件中断立即触发，完成被推迟的切换。RV32I 无
 *     “中断优先级分级”，故 sched_lock 的 prio 参数被忽略（仅保持 API 兼容）。
 *   - arch_in_isr：读 RAM 全局 g_riscv_in_trap（context.S trap 入口置 1、退出
 *     清零）。不用 mscratch：U 模式任务读该 M 模式 CSR 会触发 Illegal instruction，
 *     而读普通全局变量两特权级皆安全（RTOSUSR 自测的非特权任务也走此探测）。
 *   - arch_in_priv：软件判定（见 port.c rtos_arch_in_priv）——trap 处理器恒
 *     M 模式，任务上下文按 g_running->priv。不能读 mstatus.MPP：mret 会把 MPP
 *     清为 U，任务执行期间读 MPP 恒为 0（此前实现使临界区审计恒被跳过）。
 *     返回 0 时审计跳过 mcycle 读，规避 U 模式读 M 模式 CSR 的非法指令。
 *     注意：该函数仅可在 M 模式调用（rtos_crit_enter 先经 irq_lock 读 mstatus，
 *     U 模式任何直达路径在此之前即已由 rtos_need_svc() 导流到 SVC）。
 * ========================================================================= */

#include <stdint.h>
#include <stdbool.h>

/* RISC-V 防御钩子：强制 RTOS_MAX_ZERO_LATENCY_IRQS=0。
 * BASEPRI 临界区（mrs/msr BASEPRI）是 ARMv7-M 专有指令，RV32I 无对等物；
 * 零延迟中断方案留待后续 PLIC 优先级分级实现。即便 rtos_config.h / CMake
 * 忘记定义 0，这里也保证 rtos_internal.h 的 #if 分支走 irq_lock 路径、编译通过。
 * 原先位于 rtos_internal.h；归位到 arch 层后，任何包含本头的 TU 都会先于
 * rtos_internal.h 的 #if RTOS_MAX_ZERO_LATENCY_IRQS > 0 分支生效。 */
#if RTOS_MAX_ZERO_LATENCY_IRQS > 0
  #undef  RTOS_MAX_ZERO_LATENCY_IRQS
  #define RTOS_MAX_ZERO_LATENCY_IRQS 0
#endif

#include "riscv.h"   /* CSR 辅助 + g_riscv_in_trap + rtos_arch_in_priv 声明 */

/* 关全局中断：保存 MIE 位（1=原本开中断），清除 MIE。返回 0 或 MIE 位，
 * 与 ARM 返回 PRIMASK 的约定一致（0 = 原本已关）。 */
static inline irq_state_t irq_lock(void) {
    uint32_t ms = riscv_csr_read(RISCV_CSR_MSTATUS);
    riscv_csr_clear(RISCV_CSR_MSTATUS, RISCV_MSTATUS_MIE);
    return (ms & RISCV_MSTATUS_MIE);
}

static inline void irq_unlock(irq_state_t state) {
    if (state & RISCV_MSTATUS_MIE) {
        riscv_csr_set(RISCV_CSR_MSTATUS, RISCV_MSTATUS_MIE);
    }
}

/* 调度锁：屏蔽机器软件中断（msip 触发位）阻止任务切换，等价于 ARM 的
 * BASEPRI 屏蔽 PendSV。prio 参数在 RV32I 上无对等物，忽略（保持 API 兼容）。 */
static inline void sched_lock(uint8_t prio) {
    (void)prio;
    riscv_csr_clear(RISCV_CSR_MIE, 1u << RISCV_IRQ_SOFT);
}
/* 解调度锁：恢复 MSIE，挂起的 msip 软件中断随即触发、完成被推迟的切换 */
static inline void sched_unlock(void) {
    riscv_csr_set(RISCV_CSR_MIE, 1u << RISCV_IRQ_SOFT);
}

static inline bool irq_is_disabled(void) {
    return (riscv_csr_read(RISCV_CSR_MSTATUS) & RISCV_MSTATUS_MIE) == 0u;
}

/* 是否处于中断上下文：g_riscv_in_trap != 0（trap 期间置 1，恢复前清零） */
static inline int arch_in_isr(void) {
    return g_riscv_in_trap != 0u;
}

/* 是否特权：软件判定（port.c rtos_arch_in_priv）。
 * 不能用 mstatus.MPP==M：mret 会把 MPP 清为 U，任务执行期间恒读 0。 */
static inline int arch_in_priv(void) {
    return rtos_arch_in_priv();
}

#endif /* JOC_RTOS_ARCH_RISCV_ARCH_LOCK_H */
