#include "task_app_main.h"
#include "console.h"
#include "app_shared.h"
#include "rtos.h"
#include "iface/device.h"
#include "devmgr/device_manager.h"
#include "log/log.h"
#include "log/app_log.h"

/* 本文件是 RTOS 固件的【系统启动任务】(main)，不属于用户层 demo。
 * 职责仅限：拉起设备、填充应用上下文、挂载 Rust 应用层、跑控制台循环。
 * 任何 demo / 业务任务都在 Rust 应用层(joc-app-rust)经 rust_app_start() 创建。 */

/* Rust 应用层挂载点：由 joc-app-rust/libapp.a 提供。
 * 仅当 Rust 应用层被链接进固件时声明，RTOS 侧只负责调用；
 * 用户层 demo / 飞控示例任务全部在 Rust 层内经 ABI 契约自行创建。 */
#ifdef RUST_APP_LIB
extern void rust_app_start(app_ctx_t *ctx);
#endif

/* 主栈统一 8K：BIST/命令循环的深层调用需要。覆盖率构建曾为腾 CCM 砍到 2K，导致栈溢出、
 * 启动期 UART 输出丢失（误判 coverage 构建卡死）；现 gcov 段已搬回主 SRAM，CCM 有余量，
 * 主栈恢复 8K。 */
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

    log_printf(app_log(), LOG_INFO, "main", "jOS RTOS ready (STM32F407 Discovery, OOC)\n");

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

    /* 填充应用上下文，供命令 handler 使用（console.c 直接读 c->uart/c->adc/...） */
    c->uart      = d_uart;
    c->usb       = d_usb;
    c->led       = d_led;
    c->adc       = d_adc;
    c->temp      = d_temp;
    c->clk       = d_clk;
    c->console   = d_uart;        /* 默认控制台为 UART；USB CDC 收到命令时动态切换 */

#if RTOS_SELFTEST
    log_printf(app_log(), LOG_INFO, "main",
               "READY. Commands: PING / ECHO <text> / BIST / ADC [ch] / TEMP / TICKS / "
               "I2C_IRQ / USBOPEN / USBCLOSE / USBSTAT / USBDBG [0|1] / "
               "RTOS / RTOSIPC / RTOSBUS / RTOSMPU / RTOSSTRESS / RTOSFPU / RTOSBH / "
               "RTOSP4 / RTOSUSR / RTOSRR / RTOSKOBJ / RTOSALL / "
               "RTOSBASIC / RTOSIPC2 / RTOSROBUST\n");
#else
    log_printf(app_log(), LOG_INFO, "main",
               "READY. Commands: PING / ECHO <text> / ADC [ch] / TEMP / TICKS / "
               "I2C_IRQ / USBOPEN / USBCLOSE / USBSTAT / USBDBG [0|1] / "
               "RTOS / RTOSMARATHON / RTOSKOBJ / RTOSCOV\n"
               "  (user-layer demo / flight-ctrl example tasks are mounted by the Rust app layer at boot)\n");
#endif

#ifdef RUST_APP_LIB
    log_printf(app_log(), LOG_INFO, "main", "[boot] starting Rust app layer...\n");
    rust_app_start(c);
#else
    log_printf(app_log(), LOG_INFO, "main", "[boot] no Rust app layer linked.\n");
#endif

    console_run(c);   /* 永不返回：读命令 -> 查表派发 */
}

RTOS_TASK(main, "main", app_main_task, RTOS_PRIO_MAIN, g_main_stack, sizeof(g_main_stack), &g_app_ctx, 1);
