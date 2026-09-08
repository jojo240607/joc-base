/**
 * 系统早期初始化（从 main() 抽出，使 main() 只剩“早期初始化 + 启动调度器”）。
 *
 * 这些硬件/内核准备工作必须在任何任务运行之前完成一次：板级设备建好、1 kHz
 * systick 起好、RTOS 内核初始化并在 systick 线注册节拍。任务本身由 RTOS_TASK
 * 段注册宏在 rtos_start() 内自动实例化，无需在此手动创建。
 */
#include "board.h"
#include "system_init.h"   /* 自身头：reset_reason_t / board_* 声明 */
#include "rtos.h"
#include "log/log.h"
#include "log/app_log.h"
#if defined(STM32F103xx) || defined(STM32F1)
#include "stm32f1xx.h"    /* RCC->CSR 复位标志 */
#elif defined(STM32H750xx) || defined(STM32H7)
#include "stm32h7xx.h"    /* RCC->RSR 复位标志（H7：标志在 RSR，不在 CSR） */
#elif defined(ESP32C3)
/* RISC-V：无 RCC 复位状态寄存器；Renode 恒为上电复位 */
#else
#include "stm32f4xx.h"    /* RCC->CSR 复位标志 */
#endif

/* ---- 复位原因解码（rtos-test-plan §6.4）---- */
const char *reset_reason_name(reset_reason_t r)
{
    switch (r) {
    case RESET_REASON_POWER:    return "POWER-ON";
    case RESET_REASON_PIN:      return "PIN(NRST)";
    case RESET_REASON_SOFTWARE: return "SOFTWARE";
    case RESET_REASON_IWDG:     return "IWDG/WWDG";
    case RESET_REASON_LOWPWR:   return "LOW-PWR";
    default:                    return "UNKNOWN";
    }
}

#if !defined(ESP32C3)
reset_reason_t board_decode_reset_reason(uint32_t csr)
{
    /* STM32-specific RCC CSR decoding; ESP32-C3 has no RCC. */
    if (csr & (RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF)) return RESET_REASON_IWDG;
    if (csr & RCC_CSR_SFTRSTF)  return RESET_REASON_SOFTWARE;
    if (csr & RCC_CSR_PORRSTF)  return RESET_REASON_POWER;
    if (csr & RCC_CSR_PINRSTF)  return RESET_REASON_PIN;
    if (csr & RCC_CSR_LPWRRSTF) return RESET_REASON_LOWPWR;
    return RESET_REASON_UNKNOWN;
}
#else
/* ESP32-C3 has no RCC CSR register; stub returns POWER-ON unconditionally. */
reset_reason_t board_decode_reset_reason(uint32_t csr) {
    (void)csr;
    return RESET_REASON_POWER;
}
#endif

void board_report_reset_reason(void)
{
#if defined(STM32H750xx) || defined(STM32H7)
    uint32_t csr = RCC->RSR;                       /* H7：复位标志在 RSR */
    reset_reason_t r = board_decode_reset_reason(csr);
    log_printf(app_log(), LOG_INFO, "boot",
               "[BIST] reset : %s (RSR=0x%08lx)\n", reset_reason_name(r), csr);
    RCC->RSR |= RCC_RSR_RMVF;                      /* 写 1 清所有复位标志 */
#elif defined(ESP32C3)
    /* RISC-V 无复位原因寄存器（Renode 恒为上电复位） */
    log_printf(app_log(), LOG_INFO, "boot",
               "[BIST] reset : POWER-ON (RISC-V)\n");
#else
    uint32_t csr = RCC->CSR;
    reset_reason_t r = board_decode_reset_reason(csr);
    log_printf(app_log(), LOG_INFO, "boot",
               "[BIST] reset : %s (CSR=0x%08lx)\n", reset_reason_name(r), csr);
    RCC->CSR |= RCC_CSR_RMVF;   /* 写 1 清所有复位标志（含 RMVF 自身） */
#endif
}

void system_early_init(void)
{
    board_init();        /* 建好所有 device 并注册进 devmgr */
    board_tick_init();   /* 起 1 kHz systick */
    rtos_init();         /* 初始化内核并在 systick 线注册节拍 */
}
