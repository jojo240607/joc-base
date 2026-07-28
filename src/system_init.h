#ifndef SYSTEM_INIT_H
#define SYSTEM_INIT_H

#include <stdint.h>   /* uint32_t（复位原因 CSR 解码用） */

/* 系统早期初始化：在 rtos_start() 之前、任何任务运行之前，把硬件与内核准备好。
 * 拆成独立文件，使 main() 只做“早期初始化 + 启动调度器”两件事（任务由
 * RTOS_TASK 段注册宏在 rtos_start() 内自动实例化，无需在此手动 rtos_task_create）。 */
void system_early_init(void);

/* 复位原因（rtos-test-plan §6.4 马拉松 / 看门狗）：boot 时读取 RCC->CSR 解码，
 * 便于马拉松跑挂后定位是看门狗(IWDG/WWDG)复位、软件复位、还是上电/引脚复位。
 * STM32F4 上 IWDGRSTF 与 WWDGRSTF 是同一位（见 device/stm32f407xx.h 的别名
 * RCC_CSR_WDGRSTF），故看门狗统一解码为 RESET_REASON_IWDG。 */
typedef enum {
    RESET_REASON_UNKNOWN = 0,
    RESET_REASON_POWER,    /* PORRSTF：上电/掉电复位 */
    RESET_REASON_PIN,      /* PINRSTF：NRST 引脚复位 */
    RESET_REASON_SOFTWARE, /* SFTRSTF：软件（AIRCR 系统复位）复位 */
    RESET_REASON_IWDG,     /* IWDGRSTF（含 WWDGRSTF 别名）：独立/窗口看门狗复位 */
    RESET_REASON_LOWPWR,   /* LPWRRSTF：低功耗(退出 Standby)复位 */
} reset_reason_t;

const char    *reset_reason_name(reset_reason_t r);
reset_reason_t board_decode_reset_reason(uint32_t csr);
/* 读 RCC->CSR、打印解码结果、并写 RMVF 清除所有复位标志。
 * 在 BIST 开头调用（此处 log 后端已就绪），马拉松看门狗复位后会再次打印 IWDG。 */
void board_report_reset_reason(void);

#endif /* SYSTEM_INIT_H */
