#ifndef IRQ_HAL_H
#define IRQ_HAL_H

#include <stdint.h>
#include "irq.h"

/*
 * STM32H7 implementation of the interrupt HAL.
 *
 *   - Routes EVERY external interrupt (and SysTick) to ONE shared ISR,
 *     IRQ_CommonHandler. That ISR reads the active exception from
 *     SCB->ICSR (VECTACTIVE) and calls irq_dispatch(), so the chip's vector
 *     table and IRQ numbering never leak into drivers.
 *   - Programs the NVIC for enable / disable / priority / pending-clear.
 *
 * H750 has 150 external device IRQs (WWDG..WAKEUP_PIN) plus 16 core
 * exception slots for a total of 16 + 150 = 166.
 */

#define IRQ_HAL_TABLE_SIZE  (16 + 150)

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
