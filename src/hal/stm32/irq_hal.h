#ifndef IRQ_HAL_H
#define IRQ_HAL_H

#include <stdint.h>
#include "irq.h"

/*
 * STM32 implementation of the interrupt HAL.
 *
 *   - Routes EVERY external interrupt (and SysTick) to ONE shared ISR,
 *     IRQ_CommonHandler. That ISR reads the active exception from
 *     SCB->ICSR (VECTACTIVE) and calls irq_dispatch(), so the chip's vector
 *     table and IRQ numbering never leak into drivers.
 *   - Programs the NVIC for enable / disable / priority / pending-clear.
 *
 * The platform-independent layer (src/irq/) calls these functions; drivers
 * only ever call the generic irq_*() API and obtain their irq_id_t from their
 * own HAL (e.g. uart_hal_irq_id()), so they stay chip-agnostic.
 */

/* Number of slots the platform-independent registry needs: 16 core-exception
 * slots (0..15) + 82 external device IRQs (STM32F407: WWDG..FPU). */
#define IRQ_HAL_TABLE_SIZE  (16 + 82)

/* Map a platform interrupt id (IRQn_Type / exception number) to a linear
 * registry index. Returns -1 if the id is not dispatchable.
 * On Cortex-M the exception number == IRQn + 16 for ALL sources (core and
 * device), so the index simply IS the exception number. */
int irq_hal_index(irq_id_t id);

void irq_hal_enable(irq_id_t id);
void irq_hal_disable(irq_id_t id);
void irq_hal_set_priority(irq_id_t id, uint32_t prio);
void irq_hal_clear_pending(irq_id_t id);

/* The single shared ISR installed in every external-interrupt (and SysTick)
 * vector slot by the startup file. Declared here so the vector table can
 * reference it; defined in irq_hal.c. */
void IRQ_CommonHandler(void);

#endif /* IRQ_HAL_H */
