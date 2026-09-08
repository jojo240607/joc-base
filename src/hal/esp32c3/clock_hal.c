#include "clock_hal.h"
#include "device/esp32c3.h"      /* ESP32C3_CPU_HZ */

/*
 * ESP32-C3 clock HAL — Phase-1 Renode.
 *
 * Renode's RiscV32 core has no clock-tree model: writes to ESP32-C3 clock
 * registers are absorbed by peripheral tags, and the CPU simply runs at the
 * platform frequency (40 MHz). Both functions are therefore constant-folded.
 */

void clock_hal_configure(void)
{
    /* no clock-tree programming needed under Renode (idempotent NOP) */
}

uint32_t clock_hal_sysclk_hz(void)
{
    return ESP32C3_CPU_HZ;      /* 40000000 */
}
