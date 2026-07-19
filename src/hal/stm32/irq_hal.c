#include "irq_hal.h"
#include "irq.h"
#include <stm32f4xx.h>     /* IRQn_Type, NVIC_*, SCB, SysTick (board/chip layer) */

/*
 * STM32 interrupt HAL.
 *
 * The single shared ISR (IRQ_CommonHandler) is what makes the framework
 * portable: the vector table needs only ONE entry per slot, and the HAL
 * translates the active exception number back into an irq_id_t for dispatch.
 */

int irq_hal_index(irq_id_t id)
{
    /* exception number = IRQn + 16 for every Cortex-M source (core + device) */
    int idx = (int)id + 16;
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return -1;
    return idx;
}

void irq_hal_enable(irq_id_t id)        { NVIC_EnableIRQ((IRQn_Type)id); }
void irq_hal_disable(irq_id_t id)       { NVIC_DisableIRQ((IRQn_Type)id); }
void irq_hal_set_priority(irq_id_t id, uint32_t prio)
                                      { NVIC_SetPriority((IRQn_Type)id, (uint32_t)prio); }
void irq_hal_clear_pending(irq_id_t id) { NVIC_ClearPendingIRQ((IRQn_Type)id); }

/* The ONE ISR for all external interrupts + SysTick. The hardware has already
 * latched the active exception, so we read VECTACTIVE to learn WHICH irq fired
 * and dispatch to the registered callback. This is what lets a single handler
 * serve every vector slot in the table. */
void IRQ_CommonHandler(void)
{
    uint32_t vect = (SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk) >> SCB_ICSR_VECTACTIVE_Pos;
    irq_dispatch((irq_id_t)((int)vect - 16));
}

/* SysTick is a core (Cortex-M) peripheral, configured here in the chip HAL so
 * event-device drivers never include the raw CMSIS header. */
irq_id_t irq_hal_systick_id(void)
{
    return (irq_id_t)SysTick_IRQn;
}

int irq_hal_systick_config(uint32_t cpu_hz, uint32_t tick_hz)
{
    if (tick_hz == 0 || cpu_hz < tick_hz)
        return -1;
    return (int)SysTick_Config(cpu_hz / tick_hz);
}

void irq_hal_systick_clear(void)
{
    (void)SysTick->CTRL;        /* read clears COUNTFLAG, re-arms the tick */
}
