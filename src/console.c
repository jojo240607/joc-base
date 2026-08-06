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
#include "rtos_config.h"          /* RTOS_SELFTEST 开关：早于下方 selftest.h 门控 */
#if RTOS_SELFTEST
#include "selftest.h"
#endif
#include "board.h"
#include "drv/clock.h"
#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"
#include "drv/usb.h"
#include "drv/i2c.h"
#include "drv/timer.h"         /* TIMERDMA: timer_dma_burst / timer_get_ccr */
#include "drv/pinmux.h"
#include "drv/exti.h"           /* BTN/ BTN2 命令：软件触发按键边沿以演示上下半部 */
#include "task/task_button_wq.h" /* BTN2C 命令：读工作队列版按键中断计数 */
#include "rtos.h"
#include "rtos/rtos_mpu.h"
#include "common/gcov_dump.h"   /* §6.6 RTOSCOV：导出 gcov .gcda 帧（覆盖率构建） */
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

#if RTOS_SELFTEST
static void cmd_bist(app_ctx_t *c, const char *line)
{
    (void)line;
    log_printf(app_log(), LOG_INFO, "main",
               "BUILD: pinmux name-based (USART1_TX_PA9 / GPIOD_12 / ADC1_IN0) - %s %s\n",
               __DATE__, __TIME__);
    if (c->st) selftest_run(c->st);
}
#endif /* RTOS_SELFTEST */

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

#if RTOS_SELFTEST
/* ---- RTOS 自测类命令（统一 PASS/FAIL 回显） ---- */
static void selftest_reply(app_ctx_t *c, const char *name, int ok)
{
    char out[32];
    int n = snprintf(out, sizeof(out), "%s %s\r\n", name, ok ? "PASS" : "FAIL");
    c->console->vtable->write(c->console, out, (size_t)n);
}

static void cmd_rtosipc(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSIPC", rtos_ipc_selftest()); }
static void cmd_rtosbasic(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSBASIC", rtos_basic_selftest()); }
static void cmd_rtosipc2(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSIPC2", rtos_ipc2_selftest()); }
static void cmd_rtosrobust(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSROBUST", rtos_robust_selftest()); }
static void cmd_rtosrr (app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSRR",  rtos_rr_selftest()); }
static void cmd_rtosbus(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSBUS", rtos_bus_selftest()); }
static void cmd_rtosmpu(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSMPU", rtos_mpu_selftest()); }
static void cmd_rtosstress(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSSTRESS", rtos_stress_selftest()); }
static void cmd_rtosp4(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSP4", rtos_p4_selftest()); }
static void cmd_rtosall(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSALL", rtos_selftest_run_all()); }
static void cmd_rtosdeadline(app_ctx_t *c, const char *line) {
    (void)line;
    /* 硬实时违约报告（阶段1）：打印每个硬实时/软实时任务的 deadline/wcet/预算/违约计数，
     * 并汇总 g_rtos_deadline_violation。非实时任务(rt_class==0)不列出。 */
    int n = rtos_task_count();
    int listed = 0;
    for (int i = 0; i < n; i++) {
        uint8_t rc = rtos_task_rt_class(i);
        if (rc == 0) continue;
        const char *nm = rtos_task_name(i);
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[DEADLINE] %s class=%u prio=%u deadline=%lu wcet=%lu budget=%lu dmiss=%lu wmiss=%lu\n",
                   nm ? nm : "?", (unsigned)rc, (unsigned)rtos_task_prio(i),
                   (unsigned long)rtos_task_deadline(i), (unsigned long)rtos_task_wcet(i),
                   (unsigned long)rtos_task_budget(i),
                   (unsigned long)rtos_task_deadline_miss(i),
                   (unsigned long)rtos_task_wcet_miss(i));
        listed++;
    }
    log_printf(app_log(), LOG_INFO, "rtos",
               "[DEADLINE] listed=%d violation=%lu (deadline|wcet)\n",
               listed, (unsigned long)rtos_rt_violation());
    selftest_reply(c, "RTOSDEADLINE", rtos_rt_violation() == 0);
}
static void cmd_rtoscrit(app_ctx_t *c, const char *line) {
    (void)line;
    /* 阶段2 临界区审计报告：打印临界区超长计数（任何内核临界区持锁超
     * RTOS_CRIT_MAX_TICKS 即递增，粘性、零挂起风险）。 */
    uint32_t ov = rtos_rt_crit_overflow();
    log_printf(app_log(), LOG_INFO, "rtos",
               "[CRIT] overflow=%lu kill_count=%lu panic=%lu (max_ticks=%d, kill_mode=%d)\n",
               (unsigned long)ov,
               (unsigned long)g_rtos_crit_kill_count,
               (unsigned long)g_rtos_crit_kill_panic,
               (int)RTOS_CRIT_MAX_TICKS, (int)RTOS_CRIT_KILL);
    selftest_reply(c, "RTOSCRIT", ov == 0);
}
static void cmd_rtossched(app_ctx_t *c, const char *line) {
    /* 阶段3 可调度性静态自检报告：打印每个硬实时任务的 C/T/P/WCRT 与总体
     * 利用率、违约数（g_rtos_sched_invalid）。详细 RTA 见 rtos_sched_analysis.c。
     * P1-3：支持 `RTOSSCHED recheck` 主动调 rtos_sched_validate() 重新扫描当前
     * 任务池（运行时动态增删硬实时任务后应手动再验证），再打印最新结果。 */
    if (line && strstr(line, "recheck")) {
        int inf = rtos_sched_validate();
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[RTOSSCHED] recheck: infeasible=%d (0=all feasible)\n", inf);
    }
    rtos_sched_analysis_print();
    selftest_reply(c, "RTOSSCHED", rtos_rt_sched_invalid() == 0);
}
static void cmd_rtosfpu(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSFPU", rtos_fpu_selftest()); }
static void cmd_rtosbh(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSBH", rtos_bh_selftest()); }
static void cmd_rtostimer(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSTIMER", rtos_timer_selftest()); }
static void cmd_rtosusr(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSUSR", rtos_usr_selftest()); }
static void cmd_rtosirq(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSIRQ", rtos_irq_selftest()); }
static void cmd_rtosinv(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSINV", rtos_inv_selftest()); }
static void cmd_rtosfuzz(app_ctx_t *c, const char *line) { (void)line; selftest_reply(c, "RTOSFUZZ", rtos_fuzz_selftest()); }
static void cmd_rtosaccept(app_ctx_t *c, const char *line) {
    /* P1-4：支持 `RTOSACCEPT long` 跑 1 小时 soak（默认 60s 全 suite） */
    if (line && (strstr(line, "long") || strstr(line, "1h"))) {
        selftest_reply(c, "RTOSACCEPT_LONG", acc_b1_soak_long());
    } else {
        selftest_reply(c, "RTOSACCEPT", rtos_accept_selftest());
    }
}
#endif /* RTOS_SELFTEST */

/* §6.6 覆盖率：触发把当前累积的 gcov 计数以 .gcda 二进制帧经控制台导出。
 * host 端 tools/coverage_collect.py 连上串口、发 RTOSCOV、收帧、落盘并跑 gcov。
 * 非覆盖率构建下 gcov_dump() 是空操作，这里给一句提示。 */
/* §6.6 覆盖率：触发把累积的 gcov 计数以 .gcda 二进制帧【经调试 UART（COM8）】导出。
 * 设计上明确「.gcda 经调试 UART 透传」——USB CDC 批量 IN 在大块突发下会触发短包/stall
 * wedge（bulk_tx_pending 卡死 → 板子冻结，见 usb.c 的 usb_tx_pump），故这里【始终】走
 * gcov_dump 的默认 g_out = uart_console_raw（UART 硬件 COM8，IRQ 驱动的阻塞发送，
 * 不依赖 DMA TX 流，绝不会卡死、可重复运行）。UART 是纯字节流、无「短包=传输结束」概念；
 * uart_console_raw 保证二进制帧不做 \n->\r 转换。 */
static void cmd_rtostrace(app_ctx_t *c, const char *line)
{
    /* P2-1 调度轨迹导出：打印环形缓冲内 (tick, from, to, reason) 切换记录。
     * 关闭 RTOS_SCHED_TRACE 时此命令不在 g_cmds 注册，rtos_trace_dump 退化为 no-op。 */
    (void)c; (void)line;
#if RTOS_SCHED_TRACE
    rtos_trace_dump();
    selftest_reply(c, "RTOSTRACE", 1);
#else
    log_printf(app_log(), LOG_INFO, "rtos",
               "[RTOSTRACE] disabled (RTOS_SCHED_TRACE=0, rebuild with -DRTOS_SCHED_TRACE=1)\n");
    selftest_reply(c, "RTOSTRACE", 0);
#endif
}

static void cmd_rtosbench(app_ctx_t *c, const char *line)
{
    /* Rhealstone 子集基准 + IRQ→任务唤醒延迟直方图（RTOSBENCH 命令）。
     * 关闭 RTOS_SCHED_TRACE 时此命令不在 g_cmds 注册，rtos_bench_run 不暴露。 */
    (void)c; (void)line;
#if RTOS_SCHED_TRACE
    rtos_bench_run();
    selftest_reply(c, "RTOSBENCH", 1);
#else
    log_printf(app_log(), LOG_INFO, "rtos",
               "[RTOSBENCH] disabled (RTOS_SCHED_TRACE=0, rebuild with -DRTOS_SCHED_TRACE=1)\n");
    selftest_reply(c, "RTOSBENCH", 0);
#endif
}

static void cmd_rtoscov(app_ctx_t *c, const char *line)
{
    (void)line;
#ifdef RTOS_COVERAGE
    gcov_dump();   /* 默认经 uart_console_raw 导出到 COM8（见 gcov_dump.c） */
    const char *m = "RTOSCOV: .gcda frames sent on UART (see [GCOV DUMP END])\r\n";
    c->console->vtable->write(c->console, m, strlen(m));
#else
    const char *m = "RTOSCOV: not a coverage build (rebuild with -DCOVERAGE=ON)\r\n";
    c->console->vtable->write(c->console, m, strlen(m));
#endif
}

/* 马拉松长跑（§6.4）：派生长跑心跳任务组常驻；参数含 "wdt" 时同时 ARM 看门狗
 * （IWDG 存活至复位，仅马拉松模式用）。返回即后台运行，72h 是让它一直跑。 */
static void cmd_rtosmarathon(app_ctx_t *c, const char *line)
{
    uint8_t arm = (uint8_t)((line && strstr(line, "wdt")) ? 1 : 0);
    rtos_marathon_start(arm);
    char out[64];
    int n = snprintf(out, sizeof(out), "RTOSMARATHON START wdt=%s\r\n",
                     rtos_watchdog_is_armed() ? "ARMED" : "DISARMED");
    c->console->vtable->write(c->console, out, (size_t)n);
}

/* 软件复位：用于上位机在【不重新烧录】的情况下让板子回到全新 boot 状态，
 * 保证下一次 RTOSCOV 触发的是首次 __gcov_dump 调用（newlib gcov 首次 dump 后才
 * 填充计数；二次调用只发 START+魔法字就停 → 108 字节残帧 → 0 覆盖）。host 抓取
 * 工具在每次采集前发本命令，等价于一次硬件复位。 */
static void cmd_reset(app_ctx_t *c, const char *line)
{
    (void)line;
    const char *s = "RESET\r\n";
    c->console->vtable->write(c->console, s, strlen(s));
    for (volatile uint32_t i = 0; i < 200000; i++) { }   /* 让回显先发出去 */
    /* 软件复位：写 SCB->AIRCR（地址 0xE000ED0C），KEY=0x5FA，SYSRESETREQ=bit2。
     * 本仓库未引 CMSIS 设备头，故直接寄存器操作（Cortex-M 通用）。 */
    uint32_t *aircr = (uint32_t *)0xE000ED0CUL;
    *aircr = (0x5FAUL << 16) | (1UL << 2);
    for (;;) { }   /* 等待复位生效 */
}

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

static void cmd_btn(app_ctx_t *c, const char *line)
{
    (void)line;
    /* 软件触发 btn 的 EXTI 边沿：走真实 ISR -> 上半部(button_isr_cb) ->
     * 下半部(button_bh_fn) 全链路，无需物理按键即可演示（见 task_button.c）。 */
    device *btnd = device_manager_get("btn");
    if (!btnd) { usb_reply(c, "BTN: no dev\r\n"); return; }
    btnd->vtable->ioctl(btnd, EXTI_IOCTL_TRIGGER, NULL);
    usb_reply(c, "BTN: edge triggered (watch bottom-half log + LED)\r\n");
}

static void cmd_btnc(app_ctx_t *c, const char *line)
{
    (void)line;
    device *btnd = device_manager_get("btn");
    if (!btnd) { usb_reply(c, "BTNC: no dev\r\n"); return; }
    uint32_t cnt = 0;
    btnd->vtable->ioctl(btnd, EXTI_IOCTL_GET_COUNT, &cnt);
    char out[40];
    int n = snprintf(out, sizeof(out), "BTNC count=%lu\r\n", (unsigned long)cnt);
    c->console->vtable->write(c->console, out, (size_t)n);
}

static void cmd_btn2(app_ctx_t *c, const char *line)
{
    (void)line;
    /* 软件触发 btn2 的 EXTI 边沿：走真实 ISR -> 上半部(button2_isr_cb) ->
     * 下半部(button2_work_fn，运行在共享 wq 任务) 全链路，无需物理按键
     * （见 task_button_wq.c）。 */
    device *btnd = device_manager_get("btn2");
    if (!btnd) { usb_reply(c, "BTN2: no dev\r\n"); return; }
    btnd->vtable->ioctl(btnd, EXTI_IOCTL_TRIGGER, NULL);
    usb_reply(c, "BTN2: edge triggered (watch WORKQUEUE bottom-half log + LED)\r\n");
}

static void cmd_btn2c(app_ctx_t *c, const char *line)
{
    (void)line;
    char out[48];
    int n = snprintf(out, sizeof(out), "BTN2C count=%lu\r\n",
                     (unsigned long)button2_press_count());
    c->console->vtable->write(c->console, out, (size_t)n);
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

/* ---- UART DMA 验证命令：把控制台 UART 切到 DMA 模式，先用 DMA TX 发出一个
 * 特征串（主机若收到即证明 DMAT/路由/TC 全对），再做一个 DMA RX 收 4 字节。
 *
 * 关键在于：bulk RX 读会把 RX DMA 流配成"收满 4 字节才 TC"的阻塞传输，必须保证
 * 主机发的 4 字节确实落在这条已 armed 的 RX DMA 上。所以板子在 armed 之前先打印
 * "RX-READY"，主机看到后才发 4 字节——否则主机早先连发的字节会被交互控制台
 * （IRQ/ring）在模式切换途中消费掉，等 bulk RX armed 时线上已经没有在途字节了。
 * 主机配合：发 "UARTDMA\r\n"，读到 "RX-READY" 后立刻发 4 字节，板子回显
 * "UARTDMA RX(4)=...."。最后切回默认的 DMA_IDLE。 */
static void cmd_uartdma(app_ctx_t *c, const char *line)
{
    (void)line;
    device *u = c->uart;
    if (!u) { usb_reply(c, "UARTDMA: no uart\r\n"); return; }

    /* --- DMA TX 验证：DMA TX 成功 => DMAT/路由/TC 全对 --- */
    stream_xfer_mode_t m = STREAM_MODE_DMA;
    if (u->vtable->ioctl(u, STREAM_IOCTL_SET_MODE, &m) != 0) {
        usb_reply(c, "UARTDMA: DMA unavailable for this uart (no route)\r\n");
        return;
    }
    const char *marker = "UARTDMA_MARKER_0123456789ABCDEF\r\n";
    u->vtable->write(u, marker, strlen(marker));   /* DMA TX: host RX 即证明通路 */

    /* --- DMA RX 验证（marker 驱动）---
     * 先打印 RX-READY（DMA TX，发出后 write 才返回），再 armed bulk RX 读。主机
     * 看到 RX-READY 才发 4 字节，必落到已 armed 的 RX DMA 上（armed 在 RX-READY
     * 发完之后，避免把自身 TX 环回进 RX）。bulk 读收满 4 字节即 TC。 */
    u->vtable->write(u, "RX-READY\r\n", 10);       /* 通知主机：现在发 4 字节 */
    char rx[4];
    int n = u->vtable->read(u, rx, sizeof(rx));     /* DMA RX 读（阻塞 <=2s） */

    /* 恢复默认：engine=DMA + framing=IDLE（原 STREAM_MODE_DMA_IDLE 的等价组合） */
    stream_xfer_mode_t m2 = STREAM_MODE_DMA;
    u->vtable->ioctl(u, STREAM_IOCTL_SET_MODE, &m2);
    uart_frame_t fr = UART_FRAME_IDLE;
    u->vtable->ioctl(u, UART_IOCTL_SET_FRAMING, &fr);

    char out[120];
    int k;
    if (n == 4)
        k = snprintf(out, sizeof(out), "UARTDMA RX(4)=%c%c%c%c\r\n",
                     rx[0], rx[1], rx[2], rx[3]);
    else
        k = snprintf(out, sizeof(out), "UARTDMA RX FAIL n=%d\r\n", n);
    u->vtable->write(u, out, (size_t)k);
    usb_reply(c, n == 4 ? "UARTDMA OK (TX+RX via DMA)\r\n"
                        : "UARTDMA TX-OK RX-FAIL\r\n");
}

/* ---- TIMER DMA 验证命令：让 TIM2(timer0) 的 Update 事件驱动一条 DMA 把一组
 * 16-bit 计数搬进 CCR1，再回读 CCR1 看是否等于最后一个值。这证明 TIM2_UP 的
 * DMA 路由 + CHSEL + PAR/M0AR + TC 整条通路正确（等价于 DAC DMA 回读 DOR）。
 * TIM2 只是个 20Hz 心跳源，其 CCR1 未接到任何引脚，所以不影响心跳。 */
static void cmd_timerdma(app_ctx_t *c, const char *line)
{
    (void)line;
    device *tim = device_manager_get("timer0");   /* TIM2 */
    if (!tim) { usb_reply(c, "TIMERDMA: no timer0\r\n"); return; }
    event_device *te = device_as_event(tim);
    if (!te) { usb_reply(c, "TIMERDMA: not-event\r\n"); return; }

    tim->vtable->open(tim);
    te->vtable->enable(te);                  /* counting => overflows drive DMA */
    static const uint16_t buf[4] = { 1000, 5000, 20000, 40000 };
    int rc = timer_dma_burst((timer *)tim, 1, buf, 4);
    uint32_t ccr = timer_get_ccr((timer *)tim, 1);
    te->vtable->disable(te);
    tim->vtable->close(tim);

    char out[96];
    int n = snprintf(out, sizeof(out),
                     "TIMERDMA: CCR1=%lu (expect 40000) rc=%d %s\r\n",
                     (unsigned long)ccr, rc,
                     (rc == 0 && ccr == 40000U) ? "OK" : "FAIL");
    c->console->vtable->write(c->console, out, (size_t)n);
    usb_reply(c, (rc == 0 && ccr == 40000U) ? "TIMERDMA OK\r\n"
                                            : "TIMERDMA FAIL\r\n");
}

/* ---- 命令表：加命令只需在此追加一行 + 对应 handler ---- */
static const cmd_entry_t g_cmds[] = {
    { "PING",     cmd_ping,     0 },
    { "ECHO",     cmd_echo,     1 },
#if RTOS_SELFTEST
    { "BIST",     cmd_bist,     0 },
#endif
    { "ADC",      cmd_adc,      1 },
    { "TEMP",     cmd_temp,     0 },
    { "I2C_IRQ",  cmd_i2c_irq,  0 },
    { "TICKS",    cmd_ticks,    0 },
    { "RTOS",     cmd_rtos,     0 },
#if RTOS_SELFTEST
    { "RTOSIPC",  cmd_rtosipc,  0 },
    { "RTOSBASIC", cmd_rtosbasic, 0 },
    { "RTOSIPC2", cmd_rtosipc2, 0 },
    { "RTOSROBUST", cmd_rtosrobust, 0 },
    { "RTOSRR",   cmd_rtosrr,   0 },
    { "RTOSBUS",  cmd_rtosbus,  0 },
    { "RTOSMPU",  cmd_rtosmpu,  0 },
    { "RTOSSTRESS", cmd_rtosstress, 0 },
    { "RTOSP4",   cmd_rtosp4,   0 },
    { "RTOSALL",  cmd_rtosall,  0 },
    { "RTOSDEADLINE", cmd_rtosdeadline, 0 },
    { "RTOSCRIT",     cmd_rtoscrit,     0 },
    { "RTOSSCHED",    cmd_rtossched,    1 },
    { "RTOSFPU",  cmd_rtosfpu,  0 },
    { "RTOSBH",   cmd_rtosbh,   0 },
    { "RTOSTIMER", cmd_rtostimer, 0 },
#endif
    { "RTOSMARATHON", cmd_rtosmarathon, 0 },
#if RTOS_SELFTEST
    { "RTOSUSR",  cmd_rtosusr,  0 },
    { "RTOSIRQ",  cmd_rtosirq,  0 },
    { "RTOSINV",  cmd_rtosinv,  0 },
    { "RTOSACCEPT", cmd_rtosaccept, 1 },
    { "RTOSFUZZ", cmd_rtosfuzz, 0 },
#endif
    { "RTOSCOV",  cmd_rtoscov,  0 },   /* §6.6 覆盖率：导出 gcov .gcda 帧 */
#if RTOS_SCHED_TRACE
    { "RTOSTRACE", cmd_rtostrace, 0 }, /* P2-1 调度轨迹导出（环形缓冲 -> 调试 UART） */
    { "RTOSBENCH", cmd_rtosbench, 0 }, /* Rhealstone 子集 + IRQ→wakeup 延迟直方图 */
#endif
    { "RESET",    cmd_reset,    0 },   /* 软件复位：抓取工具在采集前发本命令回到全新 boot */
    { "RTOSKOBJ", cmd_rtoskobj, 0 },
    { "USBOPEN",  cmd_usbopen,  0 },
    { "USBCLOSE", cmd_usbclose, 0 },
    { "USBSTAT",  cmd_usbstat,  0 },
    { "USBDBG",   cmd_usbdbg,   1 },
    { "BTN",      cmd_btn,      0 },
    { "BTNC",     cmd_btnc,     0 },
    { "BTN2",     cmd_btn2,     0 },
    { "BTN2C",    cmd_btn2c,    0 },
    { "IOXFER",   cmd_ioxfer,   0 },
    { "UARTDMA",  cmd_uartdma,  0 },
    { "TIMERDMA", cmd_timerdma, 0 },
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
    /* debug: dump the offending line in hex so a misparsed command is visible */
    char dbg[64];
    int k = snprintf(dbg, sizeof(dbg), "[unk len=%d]", (int)strlen(line));
    c->console->vtable->write(c->console, dbg, (size_t)k);
    for (int i = 0; line[i]; i++) {
        k = snprintf(dbg, sizeof(dbg), " %02X", (unsigned char)line[i]);
        c->console->vtable->write(c->console, dbg, (size_t)k);
    }
    s = "\r\n";
    c->console->vtable->write(c->console, s, 2);
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
