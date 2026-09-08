/*
 * watchdog_plat.c — 平台看门狗自测（ESP32-C3）：复位原因解码契约。
 *
 * 覆盖 rtos_watchdog.c 中的弱钩子 rtos_watchdog_plat_reset_selftest()。
 * ESP32-C3 无 RCC 复位状态寄存器，board_decode_reset_reason 的 stub 恒返回
 * POWER-ON，这里验证该契约（任意 CSR 都应为 POWER）。
 * 平台自测文件，仅 ESP32-C3 平台编译。
 */
#include "system_init.h"   /* reset_reason_t / board_decode_reset_reason */
#include <stddef.h>

int rtos_watchdog_plat_reset_selftest(void)
{
    struct { uint32_t csr; reset_reason_t exp; } cases[] = {
        { (1u<<29), RESET_REASON_POWER },                      /* stub：恒 POWER-ON */
        { (1u<<31), RESET_REASON_POWER },
        { 0,        RESET_REASON_POWER },
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if (board_decode_reset_reason(cases[i].csr) != cases[i].exp) return 0;
    }
    return 1;
}
