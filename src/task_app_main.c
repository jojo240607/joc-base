#include "task_app_main.h"
#include "console.h"
#include "app_shared.h"
#include "rtos.h"
#include "iface/device.h"
#include "devmgr/device_manager.h"
#include "log/log.h"
#include "log/app_log.h"
#include "board.h"
#include "drv/clock.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"
#include "drv/usb.h"

RTOS_TASK_STACK(g_main_stack, 8192);

void app_main_task(void *arg)
{
    app_ctx_t *c = (app_ctx_t *)arg;

    /* NOTE: board_init() / board_tick_init() / rtos_init() 已由 system_early_init()
     * 在 rtos_start() 之前完成一次，故此处只 OPEN 设备 + 起 BIST + 跑控制台。 */

    /* --- unified device handles (by NAME, not by peripheral) ------------- */
    device *d_clk  = device_manager_get("clk");
    device *d_uart = device_manager_get("uart0");
    device *d_led  = device_manager_get("led");
    device *d_adc  = device_manager_get("adc0");
    device *d_temp = device_manager_get("temp0");

    /* every driver is brought up through the SAME virtual call. */
    d_clk->vtable->open(d_clk);
    d_uart->vtable->open(d_uart);
    d_led->vtable->open(d_led);
    d_adc->vtable->open(d_adc);
    d_temp->vtable->open(d_temp);

    uint32_t hz = 0;
    d_clk->vtable->ioctl(d_clk, CLK_IOCTL_GET_SYSCLK_HZ, &hz);
    log_printf(app_log(), LOG_INFO, "main", "Hello from STM32F407 Discovery (OOC) on jOS RTOS!\n");
    log_printf(app_log(), LOG_INFO, "main", "System clock: %lu Hz, USART1 @ 115200 8N1\n",
               (unsigned long)hz);
    log_printf(app_log(), LOG_INFO, "main",
               "BUILD: pinmux name-based (USART1_TX_PA9 / GPIOD_12 / ADC1_IN0) - %s %s\n",
               __DATE__, __TIME__);

    /* Bring up the CDC device EARLY and leave it connected so a real PC host can
     * enumerate it at boot — INDEPENDENT of the BIST running in its own task. */
    device *d_usb = device_manager_get("usb0");
    if (!d_usb) {
        log_printf(app_log(), LOG_INFO, "main", "[boot] usb0: NOT REGISTERED\n");
    } else if (d_usb->vtable->open(d_usb)) {
        log_printf(app_log(), LOG_INFO, "main", "[boot] usb0: OPEN FAILED\n");
    } else {
        log_printf(app_log(), LOG_INFO, "main",
                   "[boot] usb0: connected (CDC ACM, VID_0483 PID_5740)\n");
    }

    /* 填充应用上下文，供 blink / bist / 命令 handler 使用 */
    c->uart      = d_uart;
    c->usb       = d_usb;
    c->led       = d_led;
    c->adc       = d_adc;
    c->temp      = d_temp;
    c->clk       = d_clk;
    c->console   = d_uart;        /* 默认控制台为 UART；USB CDC 收到命令时动态切换 */
    g_rtos_demo_ready = 1;        /* blink 此后可安全独占 LED */

    log_printf(app_log(), LOG_INFO, "main",
               "READY. Commands: PING / ECHO <text> / BIST / ADC [ch] / TEMP / TICKS / "
               "I2C_IRQ / USBOPEN / USBCLOSE / USBSTAT / USBDBG [0|1] / "
               "RTOS / RTOSIPC / RTOSBUS / RTOSMPU / RTOSSTRESS / RTOSFPU / RTOSBH / "
               "RTOSP4 / RTOSUSR / RTOSRR / RTOSKOBJ / RTOSALL\n");

    console_run(c);   /* 永不返回：读命令 -> 查表派发 */
}

RTOS_TASK(main, "main", app_main_task, RTOS_PRIO_MAIN, g_main_stack, sizeof(g_main_stack), &g_app_ctx, 1);
