#ifndef IRQ_HAL_H
#define IRQ_HAL_H

#include <stdint.h>
#include "irq.h"

/*
 * ESP32-C3 (RISC-V) implementation of the interrupt HAL.
 *
 *   - Core exceptions (software/timer/external) are dispatched by the arch
 *     trap handler (mtvec -> _trap_handler) directly, NOT through this HAL:
 *       id 3  = machine software interrupt (scheduler, PendSV equivalent)
 *       id 7  = machine timer interrupt (RTOS tick)
 *       id 11 = machine external interrupt (PLIC -> IRQ_CommonHandler)
 *   - Peripheral sources go through the PLIC (Platform-Level Interrupt
 *     Controller @ 0x0C000000): PLIC source s (1..31) maps to irq id 16+s,
 *     so the shared ISR IRQ_CommonHandler does claim -> irq_dispatch -> complete.
 *
 * IRQ table size: 16 core slots + 32 PLIC sources = 48.
 */

#define IRQ_HAL_TABLE_SIZE  (16 + 32)

int irq_hal_index(irq_id_t id);

void irq_hal_enable(irq_id_t id);
void irq_hal_disable(irq_id_t id);
void irq_hal_set_priority(irq_id_t id, uint32_t prio);
void irq_hal_clear_pending(irq_id_t id);

void IRQ_CommonHandler(void);

irq_id_t irq_hal_systick_id(void);
int      irq_hal_systick_config(uint32_t cpu_hz, uint32_t tick_hz);
void     irq_hal_systick_clear(void);

#endif /* IRQ_HAL_H */
