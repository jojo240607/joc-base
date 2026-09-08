/*
 * watchdog_plat.c — 平台看门狗自测（STM32 家族）：复位原因解码位组合表。
 *
 * 覆盖 rtos_watchdog.c 中的弱钩子 rtos_watchdog_plat_reset_selftest()。
 * 与 RCC_CSR 一致的位号（IWDGRSTF/WWDGRSTF/SFTRSTF/PORRSTF/PINRSTF/LPWRRSTF）
 * 验证 board_decode_reset_reason 的解码优先级（IWDG>SFT、POR>PIN 等）。
 * 平台自测文件，仅 STM32 平台编译（F1/F4/H7 同族，位号一致）。
 */
#include "system_init.h"   /* reset_reason_t / board_decode_reset_reason */
#include <stddef.h>

int rtos_watchdog_plat_reset_selftest(void)
{
    struct { uint32_t csr; reset_reason_t exp; } cases[] = {
        { (1u<<29), RESET_REASON_IWDG },                       /* IWDGRSTF */
        { (1u<<30), RESET_REASON_IWDG },                       /* WWDGRSTF(=IWDG 别名) */
        { (1u<<28), RESET_REASON_SOFTWARE },                   /* SFTRSTF */
        { (1u<<27), RESET_REASON_POWER },                      /* PORRSTF */
        { (1u<<26), RESET_REASON_PIN },                        /* PINRSTF */
        { (1u<<31), RESET_REASON_LOWPWR },                     /* LPWRRSTF */
        { (1u<<26)|(1u<<27), RESET_REASON_POWER },             /* POR>PIN 优先级 */
        { (1u<<29)|(1u<<28), RESET_REASON_IWDG },              /* IWDG>SFT 优先级 */
        { 0,        RESET_REASON_UNKNOWN },
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if (board_decode_reset_reason(cases[i].csr) != cases[i].exp) return 0;
    }
    return 1;
}
