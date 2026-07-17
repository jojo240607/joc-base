#include "clock.h"
#include "stm32f4xx.h"
#include <stdlib.h>
#include <string.h>

#define CLOCK_HSE_HZ    8000000UL
#define CLOCK_SYSCLK_HZ 168000000UL

static void clock_configure(clock *self);

const struct clockFun clock_fun = {
    .destroy = clock_destroy,
    .init = clock_init,
    .deinit = clock_deinit,
    .get_sysclk_hz = clock_get_sysclk_hz,
};

clock *clock_create(void)
{
    clock *self = (clock *)malloc(sizeof(clock));
    if (!self) return NULL;
    memset(self, 0, sizeof(clock));
    clock_init(self);
    return self;
}

void clock_destroy(clock *self)
{
    if (!self) return;
    clock_deinit(self);
    free(self);
}

void clock_init(clock *self)
{
    if (!self) return;
    if (!self->vtable) {
        self->vtable = (struct clockVtable *)malloc(sizeof(struct clockVtable));
        if (self->vtable) memset(self->vtable, 0, sizeof(struct clockVtable));
    }
    self->fun = &clock_fun;
    self->vtable->configure = clock_configure;
    self->sysclk_hz = CLOCK_SYSCLK_HZ;
    clock_configure(self);
}

void clock_deinit(clock *self)
{
    if (!self) return;
    if (self->vtable) {
        free(self->vtable);
        self->vtable = NULL;
    }
}

uint32_t clock_get_sysclk_hz(clock *self)
{
    return self ? self->sysclk_hz : 0UL;
}

static void clock_configure(clock *self)
{
    (void)self;

    /* Enable HSE (8 MHz on the Discovery board) */
    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0) { }

    /* Flash latency: 5 WS @ 3.3 V, enable prefetch + I/D caches */
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_LATENCY_5WS;

    /* PLL: M=8 -> 1 MHz, N=336 -> 336 MHz, P=2 -> 168 MHz, Q=7 -> 48 MHz, SRC=HSE */
    RCC->PLLCFGR = (8U   << RCC_PLLCFGR_PLLM_Pos)
                 | (336U << RCC_PLLCFGR_PLLN_Pos)
                 | (0U   << RCC_PLLCFGR_PLLP_Pos)
                 | (7U   << RCC_PLLCFGR_PLLQ_Pos)
                 | RCC_PLLCFGR_PLLSRC_HSE;

    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0) { }

    /* Bus prescalers: AHB /1, APB1 /4 (42 MHz), APB2 /2 (84 MHz) */
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;

    /* Select PLL as system clock */
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }

    SystemCoreClock = CLOCK_SYSCLK_HZ;
}
