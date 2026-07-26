/**
 * 控制台命令解释器（从 main.c 的 ~300 行 if/else 命令链重构而来）。
 *
 * 设计：每个命令是一个独立 handler（cmd_<name>），全部登记进 g_cmds[] 命令表；
 * console_run() 读行后查表派发，main 不再有一长串 if/else。加命令 = 写一个
 * handler + 在表里加一行，不改动主循环。共享状态经 app_ctx_t 传入，避免反向
 * 依赖 main.c 的全局变量。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "log/log.h"
#include "log/app_log.h"
#include "iface/device.h"
#include "iface/stream_device.h"
#include "iface/io_xfer.h"
#include "devmgr/device_manager.h"
#include "selftest.h"
#include "board.h"
#include "drv/clock.h"
#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"
#include "drv/usb.h"
#include "drv/i2c.h"
#include "drv/pinmux.h"
#include "rtos.h"
#include "rtos/rtos_mpu.h"
#include "console.h"

/* IOXFER 异步演示完成回调：仅置标志，保持 ISR 安全。 */
static void io_demo_cb(io_xfer_t *x)
{
    if (x && x->arg)
        *(int *)x->arg = 1;
}

/* ---- 各命令 handler（line 为整条命令，含参数，由 handler 自行解析） ---- */

static void cmd_ping(app_ctx_t *c, const char *line)
{
    (void)line;
    const char *s = "PONG\r\n";
    c->console->vtable->write(c->console, s, strlen(s));
}

static void cmd_echo(app_ctx_t *c, const char *line)
{
    char out[64];
    int n = snprintf(out, sizeof(out), "%s\r\n", line + 5);
    c->console->vtable->write(c->console, out, (size_t)n);
}

static void cmd_bist(app_ctx_t *c, const char *line)
{
    (void)line;
    log_printf(app_log(), LOG_INFO, "main",
               "BUILD: pinmux name-based (USART1_TX_PA9 / GPIOD_12 / ADC1_IN0) - %s %s\n",
               __DATE__, __TIME__);
    if (c->st) selftest_run(c->st);
}

static void cmd_adc(app_ctx_t *c, const char *line)
{
    uint32_t ch = 0;
    if (line[3] == ' ') ch = (uint32_t)atoi(line + 4);
    if (ch > 18U) ch = 0U;

    c->adc->vtable->ioctl(c->adc, ADC_IOCTL_SET_CHANNEL, &ch);
    uint32_t raw = 0;
    c->adc->vtable->read(c->adc, &raw, sizeof(raw));
    uint32_t mv = 0;
    c->adc->vtable->ioctl(c->adc, ADC_IOCTL_READ_MV, &mv);
    uint32_t zero = 0U;
    c->adc->vtable->ioctl(c->adc, ADC_IOCTL_SET_CHANNEL, &zero);

    char out[64];
    int n = snprintf(out, sizeof(out), "ADC CH%lu raw=%lu mV=%lu\r\n",
                     (unsigned long)ch, (unsigned long)raw, (unsigned long)mv);
    c->console->vtable->write(c->console, out, (size_t)n);
}

static void cmd_temp(app_ctx_t *c, const char *line)
{
    (void)line;
    uint32_t traw = 0, ch = 16U;
    c->adc->vtable->ioctl(c->adc, ADC_IOCTL_SET_CHANNEL, &ch);
    c->adc->vtable->read(c->adc, &traw, sizeof(traw));
    uint32_t zero = 0U;
    c->adc->vtable->ioctl(c->adc, ADC_IOCTL_SET_CHANNEL, &zero);

    int32_t t10 = 0;
    c->temp->vtable->ioctl(c->temp, TEMP_IOCTL_READ_X10, &t10);
    uint16_t cal1 = 0, cal2 = 0;
    c->temp->vtable->ioctl(c->temp, TEMP_IOCTL_GET_CAL1, &cal1);
    c->temp->vtable->ioctl(c->temp, TEMP_IOCTL_GET_CAL2, &cal2);
    int32_t ip = t10 / 10;
    int32_t fp = (t10 < 0) ? -(t10 % 10) : (t10 % 10);

    char out[64];
    int n = snprintf(out, sizeof(out),
                     "TEMP raw=%lu cal1=%u cal2=%u C=%ld.%ld\r\n",
                     (unsigned long)traw, (unsigned)cal1, (unsigned)cal2,
                     (long)ip, (long)fp);
    c->console->vtable->write(c->console, out, (size_t)n);
}

static void cmd_i2c_irq(app_ctx_t *c, const char *line)
{
    (void)line;
    device *i2cd = device_manager_get("i2c0");
    if (!i2cd) {
        log_printf(app_log(), LOG_INFO, "main", "I2C_IRQ: no dev\n");
    } else if (i2cd->vtable->open(i2cd)) {
        log_printf(app_log(), LOG_INFO, "main", "I2C_IRQ: open FAIL\n");
    } else {
        stream_xfer_mode_t irq_m = STREAM_MODE_IRQ;
        i2cd->vtable->ioctl(i2cd, STREAM_IOCTL_SET_MODE, &irq_m);
        c->console->vtable->write(c->console, "a\n", 2);
        i2c_xfer_t ip = { .addr = 0x50, .buf = NULL, .len = 0, .result = 0 };
        int r = i2cd->vtable->ioctl(i2cd, I2C_IOCTL_MASTER_WRITE, &ip);
        c->console->vtable->write(c->console, "b\n", 2);
        log_printf(app_log(), LOG_INFO, "main", "I2C_IRQ: result=%d probe=%s\n",
                   r, ip.result == -1 ? "NACK" : "ERR");
        irq_m = STREAM_MODE_POLL;
        i2cd->vtable->ioctl(i2cd, STREAM_IOCTL_SET_MODE, &irq_m);
        i2cd->vtable->close(i2cd);
    }
}

static void cmd_ticks(app_ctx_t *c, const char *line)
{
    (void)line;
    char out[32];
    int n = snprintf(out, sizeof(out), "TICKS %lu\r\n", (unsigned long)board_ticks());
    c->console->vtable->write(c->console, out, (size_t)n);
}

static void cmd_rtos(app_ctx_t *c, const char *line)
{
    (void)line;
    char out[80];
    int n = snprintf(out, sizeof(out),
                     "RTOS tick=%lu heartbeat=%lu tasks=%d\r\n",
                     (unsigned long)rtos_tick_count(),
                     (unsigned long)(c->heartbeat ? *c->heartbeat : 0),
                     rtos_task_count());
    c->console->vtable->write(c->console, out, (size_t)n);
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
        c->console->vtable->write(c->console, out, (size_t)n);
    }
}

/* ---- RTOS 自测类命令（统一 PASS/FAIL 回显） ---- */
static void selftest_reply(app_ctx_t *c, const char *name, int ok)
{
    char out[32];
    int n = snprintf(out, sizeof(out), "%s %s\r\n", name, ok ? "PASS" : "FAIL");
    c->console->vtable->write(c->console, out, (size_t)n);
}

static void cmd_rtosipc(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSIPC", rtos_ipc_selftest()); }
static void cmd_rtosrr (app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSRR",  rtos_rr_selftest()); }
static void cmd_rtosbus(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSBUS", rtos_bus_selftest()); }
static void cmd_rtosmpu(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSMPU", rtos_mpu_selftest()); }
static void cmd_rtosstress(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSSTRESS", rtos_stress_selftest()); }
static void cmd_rtosp4(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSP4", rtos_p4_selftest()); }
static void cmd_rtosall(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSALL", rtos_selftest_run_all()); }
static void cmd_rtosfpu(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSFPU", rtos_fpu_selftest()); }
static void cmd_rtosbh(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSBH", rtos_bh_selftest()); }
static void cmd_rtosusr(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSUSR", rtos_usr_selftest()); }

static void cmd_rtoskobj(app_ctx_t *c, const char *line)
{
    (void)line;
    rtos_kobj_dump();
    const char *s = "RTOSKOBJ DUMP\r\n";
    c->console->vtable->write(c->console, s, strlen(s));
}

/* ---- USB CDC 控制台控制命令 ---- */
static void usb_reply(app_ctx_t *c, const char *msg)
{
    c->console->vtable->write(c->console, msg, strlen(msg));
}

static void cmd_usbopen(app_ctx_t *c, const char *line)
{
    (void)line;
    device *usbd = device_manager_get("usb0");
    if (!usbd) usb_reply(c, "USBOPEN: no dev\r\n");
    else if (usbd->vtable->open(usbd)) usb_reply(c, "USBOPEN: open FAIL\r\n");
    else usb_reply(c, "USBOPEN: usb0 connected (plug CN5 into PC)\r\n");
}

static void cmd_usbclose(app_ctx_t *c, const char *line)
{
    (void)line;
    device *usbd = device_manager_get("usb0");
    if (!usbd) usb_reply(c, "USBCLOSE: no dev\r\n");
    else { usbd->vtable->close(usbd); usb_reply(c, "USBCLOSE: usb0 off\r\n"); }
}

static void cmd_usbstat(app_ctx_t *c, const char *line)
{
    (void)line;
    device *usbd = device_manager_get("usb0");
    if (!usbd) usb_reply(c, "USBSTAT: no dev\r\n");
    else usbd->vtable->ioctl(usbd, USB_IOCTL_DBG_DUMP, NULL);
}

static void cmd_usbdbg(app_ctx_t *c, const char *line)
{
    int on = 0;
    if (line[6] == ' ') on = atoi(line + 7);
    device *usbd = device_manager_get("usb0");
    if (!usbd) usb_reply(c, "USBDBG: no dev\r\n");
    else {
        usbd->vtable->ioctl(usbd, USB_IOCTL_DBG_SET, &on);
        usb_reply(c, on ? "USBDBG: trace ON\r\n" : "USBDBG: trace OFF\r\n");
    }
}

static void cmd_ioxfer(app_ctx_t *c, const char *line)
{
    (void)line;
    stream_device *s = device_as_stream(c->uart);
    if (!s) {
        usb_reply(c, "ERR no stream\r\n");
        return;
    }
    static int g_io_cb_fired;
    g_io_cb_fired = 0;

    const char *m1 = "[IOXFER] sync transfer\r\n";
    io_xfer_t sx = { .buf = (void *)m1, .len = strlen(m1), .dir = IO_XFER_DIR_WRITE };
    int rs = stream_device_transfer_sync(s, &sx);

    const char *m2 = "[IOXFER] async transfer (cb)\r\n";
    io_xfer_t ax = { .buf = (void *)m2, .len = strlen(m2), .dir = IO_XFER_DIR_WRITE,
                     .callback = io_demo_cb, .arg = &g_io_cb_fired };
    int ra = stream_device_transfer_async(s, &ax);

    char out[64];
    int n = snprintf(out, sizeof(out),
                     "[IOXFER] sync r=%d done=%d | async r=%d (started)\r\n",
                     rs, (int)sx.done, ra);
    c->console->vtable->write(c->console, out, (size_t)n);

    n = snprintf(out, sizeof(out), "[IOXFER] async done=%d cb=%d\r\n",
                 (int)ax.done, g_io_cb_fired);
    c->console->vtable->write(c->console, out, (size_t)n);
}

/* ---- 命令表：加命令只需在此追加一行 + 对应 handler ---- */
static const cmd_entry_t g_cmds[] = {
    { "PING",     cmd_ping,     0 },
    { "ECHO",     cmd_echo,     1 },
    { "BIST",     cmd_bist,     0 },
    { "ADC",      cmd_adc,      1 },
    { "TEMP",     cmd_temp,     0 },
    { "I2C_IRQ",  cmd_i2c_irq,  0 },
    { "TICKS",    cmd_ticks,    0 },
    { "RTOS",     cmd_rtos,     0 },
    { "RTOSIPC",  cmd_rtosipc,  0 },
    { "RTOSRR",   cmd_rtosrr,   0 },
    { "RTOSBUS",  cmd_rtosbus,  0 },
    { "RTOSMPU",  cmd_rtosmpu,  0 },
    { "RTOSSTRESS", cmd_rtosstress, 0 },
    { "RTOSP4",   cmd_rtosp4,   0 },
    { "RTOSALL",  cmd_rtosall,  0 },
    { "RTOSFPU",  cmd_rtosfpu,  0 },
    { "RTOSBH",   cmd_rtosbh,   0 },
    { "RTOSUSR",  cmd_rtosusr,  0 },
    { "RTOSKOBJ", cmd_rtoskobj, 0 },
    { "USBOPEN",  cmd_usbopen,  0 },
    { "USBCLOSE", cmd_usbclose, 0 },
    { "USBSTAT",  cmd_usbstat,  0 },
    { "USBDBG",   cmd_usbdbg,   1 },
    { "IOXFER",   cmd_ioxfer,   0 },
};

static void dispatch(app_ctx_t *c, const char *line)
{
    for (size_t i = 0; i < sizeof(g_cmds) / sizeof(g_cmds[0]); i++) {
        const cmd_entry_t *e = &g_cmds[i];
        size_t len = strlen(e->name);
        if (e->prefix) {
            if (strncmp(line, e->name, len) == 0 &&
                (line[len] == ' ' || line[len] == '\0'))
                return e->fn(c, line);
        } else {
            if (strcmp(line, e->name) == 0)
                return e->fn(c, line);
        }
    }
    const char *s = "ERR unknown\r\n";
    c->console->vtable->write(c->console, s, strlen(s));
}

void console_run(app_ctx_t *c)
{
    char line[64];
    uint32_t idx = 0;
    for (;;) {
        char ch = 0;
        int from_usb = 0;
        if (c->uart && c->uart->vtable->read(c->uart, &ch, 1) == 1) {
            c->console = c->uart;
        } else if (c->usb && c->usb->vtable->read(c->usb, &ch, 1) == 1) {
            c->console = c->usb;        /* 命令来自 USB CDC -> 响应也走 USB */
            from_usb = 1;
        } else {
            if (c->usb) c->usb->vtable->ioctl(c->usb, USB_IOCTL_RX_REARM, NULL);
            rtos_msleep(1);
            continue;
        }

        /* 本地回显到来源端口 */
        if (from_usb) c->usb->vtable->write(c->usb, &ch, 1);
        else uart_console_putc(ch);

        if (ch == '\r' || ch == '\n') {
            if (idx > 0) {
                line[idx] = '\0';
                idx = 0;
                dispatch(c, line);
            }
        } else if (idx < (sizeof(line) - 1)) {
            line[idx++] = ch;
        }
    }
}
