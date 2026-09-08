#include "task_app_main.h"
#include "console.h"
#include "app_shared.h"
#include "rtos.h"
#include "iface/device.h"
#include "devmgr/device_manager.h"
#include "app_slot/app_slot.h"
#include "log/log.h"
#include "log/app_log.h"

/* 本文件是 RTOS 固件的【系统启动任务】(main)，不属于用户层 demo。
 * 职责仅限：拉起设备、填充应用上下文、挂载 Rust 应用层、跑控制台循环。
 * 任何 demo / 业务任务都在 Rust 应用层(joc-app-rust)经 rust_app_start() 创建。 */

/* Rust 应用层挂载点：由 joc-app-rust/libapp.a 提供。
 * 仅当 Rust 应用层被链接进固件时声明，RTOS 侧只负责调用；
 * 用户层 demo / 飞控示例任务全部在 Rust 层内经 ABI 契约自行创建。
 * App 入口无参、经 g_app_slot 服务表拿到所有能力（方案 Y 解耦）。 */
#ifdef RUST_APP_LIB
#include "rust_app.h"   /* int rust_app_start(void); app/rust 已在 RUST_APP_LIB include 路径 */

/* 兜底实现：正常情况下 rust_ticks() 由链入的 libapp.a(Rust 应用层) 提供。
 * 当前 joc-app-rust 尚未落地该符号时，此处提供定义以保链接/烧录/启动；
 * Rust 侧补齐后本兜底应删去，避免重复符号。它不改变设备注册等启动行为。 */
uint32_t rust_ticks(void) { return 0u; }
#endif

/* 主栈统一 8K：BIST/命令循环的深层调用需要。覆盖率构建曾为腾 CCM 砍到 2K，导致栈溢出、
 * 启动期 UART 输出丢失（误判 coverage 构建卡死）；现 gcov 段已搬回主 SRAM，CCM 有余量，
 * 主栈恢复 8K。
 * F103 (48KB SRAM, no CCM): reduce to 2KB to fit within limited RAM. */
#ifdef STM32F103xx
RTOS_TASK_STACK(g_main_stack, 2048);
#else
RTOS_TASK_STACK(g_main_stack, 8192);
#endif


void app_main_task(void *arg)
{
    app_ctx_t *c = (app_ctx_t *)arg;

    /* NOTE: board_init() / board_tick_init() / rtos_init() 已由 system_early_init()
     * 在 rtos_start() 之前完成一次，故此处只 OPEN 设备 + 起 BIST + 跑控制台。 */

    /* --- unified device handles (by NAME, not by peripheral) ------------- */
    device *d_clk  = device_manager_get("clk");
    device *d_uart = device_manager_get("uart0");
    device *d_led  = device_manager_get("led");

#ifdef JOC_RENODE
    /* Renode 无 STM32H7 ADC 模型（不同于 F4 的 Analog.STM32_ADC），open 会写未映射的
     * ADC 寄存器区 -> BusFault/HardFault。Renode 构建跳过 ADC / temp。 */
    device *d_adc  = NULL;
    device *d_temp = NULL;
#else
    device *d_adc  = device_manager_get("adc0");
    device *d_temp = device_manager_get("temp0");
#endif

    /* every driver is brought up through the SAME virtual call. */
    if (d_clk)  d_clk->vtable->open(d_clk);
    if (d_uart) d_uart->vtable->open(d_uart);
    if (d_led)  d_led->vtable->open(d_led);
    if (d_adc)  d_adc->vtable->open(d_adc);
    if (d_temp) d_temp->vtable->open(d_temp);

#ifdef STM32F103xx
    log_printf(app_log(), LOG_INFO, "main", "jOS RTOS ready (STM32F103RCT6, OOC)\n");
#elif defined(STM32H750xx)
    log_printf(app_log(), LOG_INFO, "main", "jOS RTOS ready (STM32H750VBT6, OOC)\n");
#elif defined(ESP32C3)
    log_printf(app_log(), LOG_INFO, "main", "jOS RTOS ready (ESP32-C3, RISC-V)\n");
#else
    log_printf(app_log(), LOG_INFO, "main", "jOS RTOS ready (STM32F407 Discovery, OOC)\n");
#endif

    /* Bring up the CDC device EARLY and leave it connected so a real PC host can
     * enumerate it at boot — INDEPENDENT of the BIST running in its own task. */
#ifdef JOC_RENODE
    /* Renode 无 USB OTG 模型（stm32f4.repl 仅有 USB:RESET tag）：open 会写未映射的
     * OTG FS 寄存器区 -> BusFault/HardFault -> WFI 挂死。Renode 构建跳过 USB。 */
    device *d_usb = NULL;
    log_printf(app_log(), LOG_INFO, "main", "[boot] usb0: SKIP (renode: no USB OTG model)\n");
#else
    device *d_usb = device_manager_get("usb0");
    if (!d_usb) {
        log_printf(app_log(), LOG_INFO, "main", "[boot] usb0: NOT REGISTERED\n");
    } else if (d_usb->vtable->open(d_usb)) {
        log_printf(app_log(), LOG_INFO, "main", "[boot] usb0: OPEN FAILED\n");
    } else {
        log_printf(app_log(), LOG_INFO, "main",
                   "[boot] usb0: connected (CDC ACM, VID_0483 PID_5740)\n");
    }
#endif

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

    /* 应用层挂载（轨 A/B 统一入口，异步任务化拉起）。
     *  - 轨 A（RUST_APP_LIB）：libapp.a 已链进本 ELF，rust_app_start 由链接器解析；
     *  - 轨 B（阶段 2 应用分区）：app_slot_load_app() 按固定地址读 APP_FLASH
     *    头部，校验 magic/abi_version，清零 App RAM 并取 entry。
     * 两条轨都在 app_slot_load_app() 内创建一个【独立的 app_host 任务】承载
     * App 入口，本函数立即返回，控制台主线程不被 App 初始化阻塞。 */
    log_printf(app_log(), LOG_INFO, "main",
               "[boot] mounting app layer (async app_host task)...\n");
    app_slot_init();
    app_slot_load_app();

    console_run(c);   /* 永不返回：读命令 -> 查表派发 */
}

RTOS_TASK(main, "main", app_main_task, RTOS_PRIO_MAIN, g_main_stack, sizeof(g_main_stack), &g_app_ctx, 1);
