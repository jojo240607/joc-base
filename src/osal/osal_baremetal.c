#include "osal/osal.h"
#include <cmsis_compiler.h>   /* __disable_irq / __enable_irq / __get_PRIMASK (port detail) */

void osal_sem_init(osal_sem_t *s, int val)
{
    if (s) s->count = val;
}

void osal_sem_wait(osal_sem_t *s)
{
    if (!s) return;
    for (;;) {
        unsigned st = osal_enter_critical();
        if (s->count > 0) {            /* atomically take one permit */
            s->count--;
            osal_exit_critical(st);
            return;
        }
        osal_exit_critical(st);
        /* spin with IRQs ENABLED so the ISR that gives can still run */
    }
}

void osal_sem_give(osal_sem_t *s)
{
    if (!s) return;
    unsigned st = osal_enter_critical();
    s->count++;                        /* atomic increment (thread or ISR) */
    osal_exit_critical(st);
}

unsigned osal_enter_critical(void)
{
    unsigned st = __get_PRIMASK();     /* bit0 == 1 => IRQs already disabled */
    __disable_irq();
    return st;
}

void osal_exit_critical(unsigned state)
{
    if (!(state & 0x1U))               /* only re-enable if they were on before */
        __enable_irq();
}
