#include "task_button.h"
#include "console.h"
#include "app_shared.h"
#include "rtos.h"
#include "rtos/core/bh.h"       /* rtos_bh_task_create / rtos_bh_trigger */
#include "iface/device.h"
#include "iface/event_device.h" /* device_as_event */
#include "devmgr/device_manager.h"
#include "drv/exti.h"           /* EXTI_IOCTL_GET_COUNT */
#include "drv/gpio_pin.h"       /* GPIO_IOCTL_TOGGLE */
#include "log/log.h"
#include "log/app_log.h"

RTOS_TASK_STACK(g_button_stack, 1024);     /* button 任务本体（仅等待，开销极小） */
RTOS_TASK_STACK(g_button_bh_stack, 1024);  /* 下半部 BH 任务栈 */

/* 下半部上下文：把按键处理需要的设备句柄打包进 BH ctx（BH 任务特权，可直接碰外设）。 */
typedef struct {
    device *led;
} button_ctx_t;

static volatile uint32_t g_btn_presses;    /* 按键次数（下半部累加） */
static button_ctx_t      g_btn_ctx;
static bh_t             *g_btn_bh;

/* ---- 下半部（任务上下文）：耗时 / 可能阻塞的逻辑都在这里 ----
 * 由上半部 rtos_bh_trigger 唤醒；可被任意更高优先级 IRQ/任务抢占。 */
static void button_bh_fn(void *arg)
{
    button_ctx_t *bc = (button_ctx_t *)arg;
    g_btn_presses++;
    if (bc->led) bc->led->vtable->ioctl(bc->led, GPIO_IOCTL_TOGGLE, (void *)0);
    log_printf(app_log(), LOG_INFO, "btn",
               "[button] bottom-half #%lu (woke by top-half)\r\n",
               (unsigned long)g_btn_presses);
}

/* ---- 上半部（中断上下文）：只做原子、快速的事 ----
 * exti 驱动在 ISR 里已清除了挂起位；这里仅触发下半部。绝不阻塞 / 忙等 / 耗时。
 * 运行在 IRQ 上下文，调用 rtos_bh_trigger（= ISR 安全的 sem_give + 请求调度）。 */
static void button_isr_cb(void *ctx, device_event_type_t ev, void *ev_data)
{
    (void)ctx; (void)ev; (void)ev_data;
    rtos_bh_trigger(g_btn_bh);
}

void button_task(void *arg)
{
    app_ctx_t *c = (app_ctx_t *)arg;
    g_btn_ctx.led = c->led;

    /* 1) 建下半部 BH 任务（高优先级 + 特权，可直接碰外设） */
    g_btn_bh = rtos_bh_task_create("btn_bh", RTOS_PRIO_BH_HIGH,
                                   g_button_bh_stack, sizeof(g_button_bh_stack),
                                   button_bh_fn, &g_btn_ctx);
    if (!g_btn_bh) {
        log_printf(app_log(), LOG_INFO, "btn", "[button] BH create FAIL\r\n");
        for (;;) rtos_yield();
    }

    /* 2) 取按键设备（板子须注册名为 "btn" 的 exti 设备，见 board.c） */
    device *btn = device_manager_get("btn");
    if (!btn) {
        log_printf(app_log(), LOG_INFO, "btn", "[button] no 'btn' device — idle\r\n");
        for (;;) rtos_yield();
    }

    /* 3) open：经 pinmux 申请引脚、路由 EXTI、注册 ISR、设优先级 */
    btn->vtable->open(btn);
    /* 4) 订阅中断回调（该回调运行在 ISR 上下文 = 上半部） */
    event_device *ev = device_as_event(btn);
    ev->vtable->set_event_callback(ev, DEVICE_EVENT_IRQ, button_isr_cb, NULL);
    /* 5) 武装 NVIC，等待按键边沿 */
    ev->vtable->enable(ev);

    log_printf(app_log(), LOG_INFO, "btn", "[button] ready — BTN 命令可软件触发边沿(PA2)\r\n");
    /* 任务本体空闲：所有按键工作在 BH 任务里完成；此处阻塞让出 CPU（不空转）。 */
    for (;;) rtos_msleep(1000);
}

RTOS_TASK(button, "button", button_task, 20, g_button_stack, sizeof(g_button_stack), &g_app_ctx, 1);
