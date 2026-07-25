/**
 * STM32F4 Discovery (STM32F407VGT6) minimal OOC example — running on jOS RTOS.
 *
 * 启动流程：
 *   Reset -> board_init() 建好所有 device -> board_tick_init() 起 1kHz systick
 *        -> rtos_init() 初始化内核并在 systick 线注册节拍
 *        -> 创建 main/blink/idle 三个任务 -> rtos_start() 切到首个任务。
 *
 * 原裸机 main() 的全部逻辑（开设备、BIST、命令循环）现在跑在 "main" 任务里；
 * blink 任务是高优先级演示任务，idle 任务是永远就绪的最低优先级空闲任务。
 * 统一驱动接口、irq 框架、devmgr 完全不动；osal_sem 已被 rtos 版接管为
 * “阻塞任务”，驱动阻塞路径零改动。
 */
#include <stdio.h>
#include "log/log.h"
#include "log/app_log.h"
#include <string.h>
#include <stdlib.h>
#include "iface/device.h"
#include "iface/stream_device.h"
#include "iface/io_xfer.h"
#include "devmgr/device_manager.h"
#include "board.h"
#include "drv/clock.h"
#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"
#include "drv/pinmux.h"
#include "drv/usb.h"
#include "drv/i2c.h"
#include "selftest.h"
#include "rtos.h"
#include "rtos/rtos_mpu.h"

/* completion callback for the IOXFER async demo: records that the transfer
 * finished. Runs in ISR/thread context depending on the engine; just sets a
 * flag so it stays ISR-safe. */
static void io_demo_cb(io_xfer_t *x)
{
    if (x && x->arg)
        *(int *)x->arg = 1;
}

/* ----- 任务栈（静态分配，8 字节对齐以满足异常栈约束） -----
 * 注意：裸机时 main 用的是巨大的 MSP 栈；RTOS 下每个任务有独立栈，
 * 必须给够。app_main_task 跑 BIST + printf，栈需求很大，给 8 KB；
 * blink/idle 很小，分别给 1 KB / 512 B。布局上 g_main_stack 紧邻
 * g_blink_stack 上方，栈向下生长，main 栈不够会踩坏 blink 栈导致 HardFault。 */
static uint8_t g_main_stack[8192] __attribute__((aligned(8)));
static uint8_t g_blink_stack[1024] __attribute__((aligned(8)));
static uint8_t g_idle_stack[512]  __attribute__((aligned(8)));
static uint8_t g_bist_stack[3072] __attribute__((aligned(8)));  /* BIST 后台任务栈 */

static volatile uint32_t g_heartbeat = 0;       /* blink 任务心跳计数 */
static volatile int      g_rtos_demo_ready = 0; /* BIST 完成后才允许 blink 动 LED */
static device *g_led = (device *)0;
static device *g_console = NULL;                /* 当前命令控制台(UART 或 USB CDC) */
static selftest *g_st = NULL;                   /* 共享自测句柄(BIST 命令 + bist 任务) */

/* 高优先级演示任务：BIST 完成前只在等标志（不碰 LED，避免干扰 LED 自测），
 * 之后每 500ms 翻转 LED 并递增心跳计数，证明高优先级任务能抢占 main。 */
static void blink_task(void *arg)
{
    (void)arg;
    while (!g_rtos_demo_ready) rtos_msleep(10);
    for (;;) {
        if (g_led) g_led->vtable->ioctl(g_led, GPIO_IOCTL_TOGGLE, (void *)0);
        g_heartbeat++;
        rtos_msleep(500);
    }
}

/* 最低优先级空闲任务：永远 READY，主动让出；保证就绪队列永不为空。 */
static void idle_task(void *arg)
{
    (void)arg;
    for (;;) rtos_yield();
}

/* BIST 后台任务：在独立的【低优先级】任务里跑板级自测，避免其(已知会卡死的
 * selftest_vtimer 等)阻塞 main 任务的 USB CDC / UART 控制台。任务挂起时主动让出
 * CPU，不浪费；控制台始终在更高优先级上保持响应。BIST 修复后此任务自然跑完。
 * 注意：BIST 子测试除 vusb 外不重复 open 设备，故与已打开的控制台设备无冲突；
 * vusb 已加保护，不会再 close 掉正在用的控制台 USB。 */
static void bist_task(void *arg)
{
    (void)arg;
    device *d_clk  = device_manager_get("clk");
    device *d_uart = device_manager_get("uart0");
    device *d_led  = device_manager_get("led");
    device *d_adc  = device_manager_get("adc0");
    device *d_temp = device_manager_get("temp0");

    g_st = selftest_create(d_clk, d_uart, d_led, d_adc, d_temp);
    selftest_run(g_st);

    /* pinmux conflict-detection self-test (exercises the new driver) */
    device *d_pinmux = device_manager_get("pinmux");
    int pmok = pinmux_run_selftest((pinmux *)d_pinmux);
    log_printf(app_log(), LOG_INFO, "main", "[BIST] pinmux: %s\n", pmok ? "PASS" : "FAIL");

    for (;;) rtos_yield();   /* BIST 完成(或卡死在上面)；在此安静让出 */
}

/* 原裸机 main() 的全部逻辑，现作为 "main" 任务运行。 */
static void app_main_task(void *arg)
{
    (void)arg;

    /* NOTE: board_init() / board_tick_init() are already done ONCE in main()
     * BEFORE rtos_start() (so the clock, devices and 1 kHz systick exist before
     * any task runs). Doing them again here would re-init the PLL/pinmux on a
     * live system and corrupt the UART baud — so we only OPEN + BIST here. */

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

    log_printf(app_log(), LOG_INFO, "main", "BUILD: pinmux name-based (USART1_TX_PA9 / GPIOD_12 / ADC1_IN0) - %s %s\n",
               __DATE__, __TIME__);

    /* Bring up the CDC device EARLY and leave it connected so a real PC host can
     * enumerate it at boot — INDEPENDENT of the (deferred) BIST hang. The USB
     * console then works even while BIST runs/hangs in its own background task. */
    device *d_usb = device_manager_get("usb0");
    if (!d_usb) {
        log_printf(app_log(), LOG_INFO, "main", "[boot] usb0: NOT REGISTERED\n");
    } else if (d_usb->vtable->open(d_usb)) {
        log_printf(app_log(), LOG_INFO, "main", "[boot] usb0: OPEN FAILED\n");
    } else {
        log_printf(app_log(), LOG_INFO, "main", "[boot] usb0: connected (CDC ACM, VID_0483 PID_5740)\n");
    }

    /* Spawn BIST in a low-priority background task so it can NEVER block the
     * console. (BIST currently hangs in selftest_vtimer — deferred fix.) */
    rtos_task_create("bist", bist_task, (void *)0, RTOS_PRIO_BIST, g_bist_stack, sizeof(g_bist_stack));

    /* blink 任务此后可安全独占 LED（BIST 在后台跑，不阻塞） */
    g_led = d_led;
    g_console = d_uart;        /* 默认控制台为 UART；USB CDC 收到命令时动态切换 */
    g_rtos_demo_ready = 1;

    log_printf(app_log(), LOG_INFO, "main", "READY. Commands: PING / ECHO <text> / BIST / ADC [ch] / TEMP / TICKS / I2C_IRQ / USBOPEN / USBCLOSE / USBSTAT / USBDBG [0|1] / RTOS / RTOSIPC / RTOSMPU / RTOSSTRESS / RTOSFPU / RTOSALL\n");

    /* command loop (PC companion test exercises this). Console input is accepted
     * from BOTH the debug UART and the USB CDC-ACM port; the command response is
     * written back to whichever port issued the command (g_console). */
    char line[64];
    uint32_t idx = 0;
    while (1)
    {
        char c = 0;
        int from_usb = 0;
        if (d_uart->vtable->read(d_uart, &c, 1) == 1) {
            g_console = d_uart;
        } else if (d_usb && d_usb->vtable->read(d_usb, &c, 1) == 1) {
            g_console = d_usb;        /* 命令来自 USB CDC -> 响应也走 USB */
            from_usb = 1;
        } else {
            /* 无输入：保持 bulk-OUT 端点 armed（背压恢复），并让出 1ms */
            if (d_usb) d_usb->vtable->ioctl(d_usb, USB_IOCTL_RX_REARM, NULL);
            rtos_msleep(1);
            continue;
        }

        /* 本地回显到来源端口 */
        if (from_usb) d_usb->vtable->write(d_usb, &c, 1);
        else uart_console_putc(c);

        if (c == '\r' || c == '\n')
        {
            if (idx > 0)
            {
                line[idx] = '\0';
                idx = 0;

                if (strcmp(line, "PING") == 0)
                {
                    g_console->vtable->write(g_console, "PONG\r\n", 6);
                }
                else if (strncmp(line, "ECHO ", 5) == 0)
                {
                    char out[64];
                    int n = snprintf(out, sizeof(out), "%s\r\n", line + 5);
                    g_console->vtable->write(g_console, out, (size_t)n);
                }
                else if (strcmp(line, "BIST") == 0)
                {
                    log_printf(app_log(), LOG_INFO, "main", "BUILD: pinmux name-based (USART1_TX_PA9 / GPIOD_12 / ADC1_IN0) - %s %s\n",
                               __DATE__, __TIME__);
                    if (g_st) selftest_run(g_st);
                }
                else if (strncmp(line, "ADC", 3) == 0)
                {
                    uint32_t ch = 0;
                    if (line[3] == ' ')
                        ch = (uint32_t)atoi(line + 4);
                    if (ch > 18U) ch = 0U;

                    d_adc->vtable->ioctl(d_adc, ADC_IOCTL_SET_CHANNEL, &ch);
                    uint32_t raw = 0;
                    d_adc->vtable->read(d_adc, &raw, sizeof(raw));
                    uint32_t mv = 0;
                    d_adc->vtable->ioctl(d_adc, ADC_IOCTL_READ_MV, &mv);
                    uint32_t zero = 0U;
                    d_adc->vtable->ioctl(d_adc, ADC_IOCTL_SET_CHANNEL, &zero);

                    char out[64];
                    int n = snprintf(out, sizeof(out),
                                     "ADC CH%lu raw=%lu mV=%lu\r\n",
                                     (unsigned long)ch,
                                     (unsigned long)raw, (unsigned long)mv);
                    g_console->vtable->write(g_console, out, (size_t)n);
                }
                else if (strcmp(line, "TEMP") == 0)
                {
                    uint32_t traw = 0;
                    uint32_t ch = 16U;
                    d_adc->vtable->ioctl(d_adc, ADC_IOCTL_SET_CHANNEL, &ch);
                    d_adc->vtable->read(d_adc, &traw, sizeof(traw));
                    uint32_t zero = 0U;
                    d_adc->vtable->ioctl(d_adc, ADC_IOCTL_SET_CHANNEL, &zero);

                    int32_t t10 = 0;
                    d_temp->vtable->ioctl(d_temp, TEMP_IOCTL_READ_X10, &t10);
                    uint16_t cal1 = 0, cal2 = 0;
                    d_temp->vtable->ioctl(d_temp, TEMP_IOCTL_GET_CAL1, &cal1);
                    d_temp->vtable->ioctl(d_temp, TEMP_IOCTL_GET_CAL2, &cal2);
                    int32_t ip = t10 / 10;
                    int32_t fp = (t10 < 0) ? -(t10 % 10) : (t10 % 10);

                    char out[64];
                    int n = snprintf(out, sizeof(out),
                                     "TEMP raw=%lu cal1=%u cal2=%u C=%ld.%ld\r\n",
                                     (unsigned long)traw,
                                     (unsigned)cal1,
                                     (unsigned)cal2,
                                     (long)ip, (long)fp);
                    g_console->vtable->write(g_console, out, (size_t)n);
                }
                else if (strcmp(line, "I2C_IRQ") == 0)
                {
                    device *i2cd = device_manager_get("i2c0");
                    if (!i2cd) { log_printf(app_log(), LOG_INFO, "main", "I2C_IRQ: no dev\n"); }
                    else if (i2cd->vtable->open(i2cd)) { log_printf(app_log(), LOG_INFO, "main", "I2C_IRQ: open FAIL\n"); }
                    else {
                        stream_xfer_mode_t irq_m = STREAM_MODE_IRQ;
                        i2cd->vtable->ioctl(i2cd, STREAM_IOCTL_SET_MODE, &irq_m);
                        g_console->vtable->write(g_console, "a\n", 2);
                        i2c_xfer_t ip = { .addr = 0x50, .buf = NULL, .len = 0, .result = 0 };
                        int r = i2cd->vtable->ioctl(i2cd, I2C_IOCTL_MASTER_WRITE, &ip);
                        g_console->vtable->write(g_console, "b\n", 2);
                        log_printf(app_log(), LOG_INFO, "main", "I2C_IRQ: result=%d probe=%s\n", r, ip.result == -1 ? "NACK" : "ERR");
                        irq_m = STREAM_MODE_POLL;
                        i2cd->vtable->ioctl(i2cd, STREAM_IOCTL_SET_MODE, &irq_m);
                        i2cd->vtable->close(i2cd);
                    }
                }
                else if (strcmp(line, "TICKS") == 0)
                {
                    char out[32];
                    int n = snprintf(out, sizeof(out), "TICKS %lu\r\n",
                                     (unsigned long)board_ticks());
                    g_console->vtable->write(g_console, out, (size_t)n);
                }
                else if (strcmp(line, "RTOS") == 0)
                {
                    char out[80];
                    int n = snprintf(out, sizeof(out),
                                     "RTOS tick=%lu heartbeat=%lu tasks=%d\r\n",
                                     (unsigned long)rtos_tick_count(),
                                     (unsigned long)g_heartbeat,
                                     rtos_task_count());
                    g_console->vtable->write(g_console, out, (size_t)n);
                    for (int i = 0; i < rtos_task_count(); i++) {
                        const char *ststr = "?";
                        switch (rtos_task_state(i)) {
                            case TASK_READY:    ststr = "READY"; break;
                            case TASK_RUNNING:  ststr = "RUN";   break;
                            case TASK_BLOCKED:  ststr = "BLK";   break;
                            case TASK_SLEEPING: ststr = "SLEEP"; break;
                            case TASK_DEAD:     ststr = "DEAD";  break;
                        }
                        n = snprintf(out, sizeof(out), "  %-6s prio=%2u %s\r\n",
                                     rtos_task_name(i) ? rtos_task_name(i) : "?",
                                     (unsigned)rtos_task_prio(i), ststr);
                        g_console->vtable->write(g_console, out, (size_t)n);
                    }
                }
                else if (strcmp(line, "RTOSIPC") == 0)
                {
                    int ipcok = rtos_ipc_selftest();
                    char out[32];
                    int n = snprintf(out, sizeof(out), "RTOSIPC %s\r\n", ipcok ? "PASS" : "FAIL");
                    g_console->vtable->write(g_console, out, (size_t)n);
                }
                else if (strcmp(line, "RTOSMPU") == 0)
                {
                    int mpuok = rtos_mpu_selftest();
                    char out[32];
                    int n = snprintf(out, sizeof(out), "RTOSMPU %s\r\n", mpuok ? "PASS" : "FAIL");
                    g_console->vtable->write(g_console, out, (size_t)n);
                }
                else if (strcmp(line, "RTOSSTRESS") == 0)
                {
                    int sok = rtos_stress_selftest();
                    char out[32];
                    int n = snprintf(out, sizeof(out), "RTOSSTRESS %s\r\n", sok ? "PASS" : "FAIL");
                    g_console->vtable->write(g_console, out, (size_t)n);
                }
                else if (strcmp(line, "RTOSALL") == 0)
                {
                    int allok = rtos_selftest_run_all();
                    char out[32];
                    int n = snprintf(out, sizeof(out), "RTOSALL %s\r\n", allok ? "PASS" : "FAIL");
                    g_console->vtable->write(g_console, out, (size_t)n);
                }
                else if (strcmp(line, "RTOSFPU") == 0)
                {
                    int fok = rtos_fpu_selftest();
                    char out[32];
                    int n = snprintf(out, sizeof(out), "RTOSFPU %s\r\n", fok ? "PASS" : "FAIL");
                    g_console->vtable->write(g_console, out, (size_t)n);
                }
                else if (strcmp(line, "USBOPEN") == 0)
                {
                    device *usbd = device_manager_get("usb0");
                    if (!usbd) { g_console->vtable->write(g_console, "USBOPEN: no dev\r\n", 18); }
                    else if (usbd->vtable->open(usbd)) {
                        g_console->vtable->write(g_console, "USBOPEN: open FAIL\r\n", 20);
                    } else {
                        g_console->vtable->write(g_console,
                            "USBOPEN: usb0 connected (plug CN5 into PC)\r\n", 43);
                    }
                }
                else if (strcmp(line, "USBCLOSE") == 0)
                {
                    device *usbd = device_manager_get("usb0");
                    if (!usbd) { g_console->vtable->write(g_console, "USBCLOSE: no dev\r\n", 19); }
                    else { usbd->vtable->close(usbd);
                           g_console->vtable->write(g_console, "USBCLOSE: usb0 off\r\n", 20); }
                }
                else if (strcmp(line, "USBSTAT") == 0)
                {
                    device *usbd = device_manager_get("usb0");
                    if (!usbd) { g_console->vtable->write(g_console, "USBSTAT: no dev\r\n", 18); }
                    else { usbd->vtable->ioctl(usbd, USB_IOCTL_DBG_DUMP, NULL); }
                }
                else if (strncmp(line, "USBDBG", 6) == 0)
                {
                    int on = 0;
                    if (line[6] == ' ') on = atoi(line + 7);
                    device *usbd = device_manager_get("usb0");
                    if (!usbd) { g_console->vtable->write(g_console, "USBDBG: no dev\r\n", 17); }
                    else {
                        usbd->vtable->ioctl(usbd, USB_IOCTL_DBG_SET, &on);
                        g_console->vtable->write(g_console,
                            on ? "USBDBG: trace ON\r\n" : "USBDBG: trace OFF\r\n",
                            on ? 17 : 18);
                    }
                }
                else if (strcmp(line, "IOXFER") == 0)
                {
                    stream_device *s = device_as_stream(d_uart);
                    if (!s) {
                        g_console->vtable->write(g_console, "ERR no stream\r\n", 14);
                    } else {
                        static int g_io_cb_fired;
                        g_io_cb_fired = 0;

                        const char *m1 = "[IOXFER] sync transfer\r\n";
                        io_xfer_t sx = { .buf = (void *)m1, .len = strlen(m1),
                                         .dir = IO_XFER_DIR_WRITE };
                        int rs = stream_device_transfer_sync(s, &sx);

                        const char *m2 = "[IOXFER] async transfer (cb)\r\n";
                        io_xfer_t ax = { .buf = (void *)m2, .len = strlen(m2),
                                         .dir = IO_XFER_DIR_WRITE,
                                         .callback = io_demo_cb,
                                         .arg = &g_io_cb_fired };
                        int ra = stream_device_transfer_async(s, &ax);

                        char out[64];
                        int n = snprintf(out, sizeof(out),
                                         "[IOXFER] sync r=%d done=%d | async r=%d (started)\r\n",
                                         rs, (int)sx.done, ra);
                        g_console->vtable->write(g_console, out, (size_t)n);

                        n = snprintf(out, sizeof(out),
                                     "[IOXFER] async done=%d cb=%d\r\n",
                                     (int)ax.done, g_io_cb_fired);
                        g_console->vtable->write(g_console, out, (size_t)n);
                    }
                }
                else
                {
                    g_console->vtable->write(g_console, "ERR unknown\r\n", 13);
                }
            }
        }
        else if (idx < (sizeof(line) - 1))
        {
            line[idx++] = c;
        }
    }
}

int main(void)
{
    /* 仅做最小硬件初始化 + 启动 RTOS；其余逻辑在 app_main_task 里。 */
    board_init();
    board_tick_init();

    rtos_init();
    rtos_task_create("main",  app_main_task, (void *)0, RTOS_PRIO_MAIN,  g_main_stack,  sizeof(g_main_stack));
    rtos_task_create("blink", blink_task,    (void *)0, RTOS_PRIO_BLINK, g_blink_stack, sizeof(g_blink_stack));
    rtos_task_create("idle",  idle_task,     (void *)0, RTOS_PRIO_IDLE,  g_idle_stack,  sizeof(g_idle_stack));

    rtos_start();   /* 切换到首个任务；此线程上下文被丢弃，不再返回 */

    for (;;) { }    /* 保险：rtos_start 不会返回 */
}
