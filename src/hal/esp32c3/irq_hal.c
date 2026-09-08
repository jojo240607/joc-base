#include "irq_hal.h"
#include "irq.h"
#include "riscv.h"                  /* PLIC registers, RISCV_CSR_MIE */
#include "arch/rtos_arch.h"        /* rtos_arch_tick_start() */
#include "device/esp32c3.h"         /* ESP32C3_IRQ_* source numbers */

/*
 * ESP32-C3 interrupt HAL (RISC-V).
 *
 * Core interrupt slots (irq id < 16) are RISC-V machine interrupts; their
 * enable bit lives in the mie CSR (3=software, 7=timer, 11=external). The
 * trap handler (context.S) dispatches them directly — the HAL only mirrors
 * mie for enable/disable so the generic framework stays uniform.
 *
 * Peripheral sources (irq id >= 16) are PLIC sources: source s = id - 16.
 * The PLIC's machine-external line (mie bit 11) fans them into the trap
 * handler, which calls IRQ_CommonHandler: claim -> irq_dispatch -> complete.
 *
 * Priority: the PLIC is a fixed-priority arbiter — priority 0 means "never
 * claimed". All IRQ_PRIO_* bands in this project are non-zero (2/5/8), so a
 * priority write is a direct PLIC_PRIORITY[src] store.
 */

/* PLIC has 1 context (single-hart M-mode). Source 0 is reserved by spec.
 *
 * Enable-bit mapping: the RISC-V spec numbers enable bit (s-1) for source s,
 * but the Renode PLIC model (PlatformLevelInterruptController,
 * AddContextEnablesRegister) maps bit N DIRECTLY to source N (sourceIdBase =
 * offset*8, then bit index = source number). UART0 is source 13, so the
 * context-0 enable word must carry bit 13 (0x2000) — NOT bit 12. With the
 * spec-style (src-1) mapping the UART source is never enabled: MarkSourceAsPending
 * only marks enabled sources, so claim stays 0 and no external trap ever fires
 * (observed: console TX deadlock, PLIC claim = 0, 16550 IIR pending). */
#define PLIC_SRC_FROM_ID(id)    ((uint32_t)(id) - RISCV_IRQ_EXTERNAL_BASE)
#define PLIC_EN_BIT(src)        (1u << (src))

int irq_hal_index(irq_id_t id)
{
    if (id < 0 || id >= (irq_id_t)IRQ_HAL_TABLE_SIZE)
        return -1;
    return (int)id;
}

void irq_hal_enable(irq_id_t id)
{
    if (id < 0) return;
    if (id < (irq_id_t)RISCV_IRQ_EXTERNAL_BASE) {
        /* core slot: mirror the mie bit (id 3/7/11) */
        riscv_csr_set(RISCV_CSR_MIE, (1u << (uint32_t)id));
    } else {
        uint32_t src = PLIC_SRC_FROM_ID(id);
        if (src == 0u || src > 31u) return;
        /* enable the source for context 0 and keep the PLIC threshold at 0
         * (all priorities pass; Renode PLIC defaults to 0 anyway) */
        *(volatile uint32_t *)RISCV_PLIC_ENABLE |= PLIC_EN_BIT(src);
        *(volatile uint32_t *)RISCV_PLIC_THRESHOLD = 0u;
        riscv_csr_set(RISCV_CSR_MIE, 1u << RISCV_IRQ_EXT);
    }
}

void irq_hal_disable(irq_id_t id)
{
    if (id < 0) return;
    if (id < (irq_id_t)RISCV_IRQ_EXTERNAL_BASE) {
        riscv_csr_clear(RISCV_CSR_MIE, (1u << (uint32_t)id));
    } else {
        uint32_t src = PLIC_SRC_FROM_ID(id);
        if (src == 0u || src > 31u) return;
        *(volatile uint32_t *)RISCV_PLIC_ENABLE &= ~PLIC_EN_BIT(src);
    }
}

void irq_hal_set_priority(irq_id_t id, uint32_t prio)
{
    if (id < (irq_id_t)RISCV_IRQ_EXTERNAL_BASE)
        return;                         /* core interrupts have no PLIC priority */
    uint32_t src = PLIC_SRC_FROM_ID(id);
    if (src == 0u || src > 31u) return;
    *(volatile uint32_t *)(RISCV_PLIC_PRIORITY + 4u * src) = prio;
}

void irq_hal_clear_pending(irq_id_t id)
{
    /* RISC-V has no NVIC-style pending-clear: a pending PLIC source is
     * deasserted by claim (read of CLAIM), a pending core interrupt by
     * servicing it (msip write / mtimecmp advance). No-op. */
    (void)id;
}

/* Shared PLIC ISR (called from context.S .Ltrap_external).
 * Loop until no source remains: claim returns the highest-priority pending
 * source (or 0), dispatch it, then write it back to COMPLETE. */
void IRQ_CommonHandler(void)
{
    for (;;) {
        uint32_t claim = *(volatile uint32_t *)RISCV_PLIC_CLAIM;
        if (claim == 0u) break;
        irq_dispatch((irq_id_t)(RISCV_IRQ_EXTERNAL_BASE + claim));
        *(volatile uint32_t *)RISCV_PLIC_COMPLETE = claim;
    }
}

irq_id_t irq_hal_systick_id(void)
{
    return (irq_id_t)RISCV_IRQ_TIMER;   /* 7 — machine timer */
}

/* RTOS tick is driven by the CLINT mtimecmp (arch layer owns it); the HAL
 * just delegates — no SysTick-config equivalent exists on RISC-V. */
int irq_hal_systick_config(uint32_t cpu_hz, uint32_t tick_hz)
{
    (void)cpu_hz; (void)tick_hz;
    rtos_arch_tick_start();
    return 0;
}

void irq_hal_systick_clear(void)
{
    /* mtimecmp keeps counting; nothing to clear. */
}
