#include "task_button_wq.h"
#include "console.h"
#include "app_shared.h"
#include "rtos.h"
#include "rtos/core/bh.h"       /* rtos_work_submit / rtos_work_t（共享工作队列） */
#include "iface/device.h"
#include "iface/event_device.h" /* device_as_event */
#include "devmgr/device_manager.h"
#include "drv/exti.h"           /* EXTI_IOCTL_GET_COUNT */
#include "drv/gpio_pin.h"       /* GPIO_IOCTL_TOGGLE */
#include "log/log.h"
#include "log/app_log.h"

RTOS_TASK_STACK(g_button_wq_stack, 1024);   /* 仅"拥有任务"本身：setup + 阻塞驻留 */

/* 下半部上下文：把按键处理需要的设备句柄打包（wq 任务特权，可直接碰外设）。 */
typedef struct {
    device *led;
} button2_ctx_t;

/* 工作节点：必须【长期存活】，因为提交后由 wq worker 在将来出队执行。
 * 单一常驻节点反复提交即可（多次快速触发会自环入队，worker 的排空循环会各处理一次）。 */
static rtos_work_t     g_btn2_work;
static button2_ctx_t   g_btn2_ctx;
static volatile uint32_t g_btn2_presses;

uint32_t button2_press_count(void) { return g_btn2_presses; }

/* ---- 下半部（运行在共享 wq 任务，RTOS_PRIO_BH_MED，任务模式）----
 * 由上半部 rtos_work_submit 入队，wq worker 出队后调用本函数。
 * 可做耗时/阻塞逻辑（翻转 LED、协议解析、发消息）；可被更高优先级 IRQ/任务抢占。
 * 注意：这里不再是一个"本任务的回调"，而是内核 wq 线程跑的函数。 */
static void button2_work_fn(void *arg)
{
    button2_ctx_t *bc = (button2_ctx_t *)arg;
    g_btn2_presses++;
    if (bc->led) bc->led->vtable->ioctl(bc->led, GPIO_IOCTL_TOGGLE, (void *)0);
    log_printf(app_log(), LOG_INFO, "btn2",
               "[button2] bottom-half via WORKQUEUE #%lu (ran on shared 'wq' task)\r\n",
               (unsigned long)g_btn2_presses);
}

/* ---- 上半部（中断上下文）：只做原子、快速的事 ----
 * exti 驱动在 ISR 里已清挂起位；这里仅把工作挂入共享队列。绝不阻塞 / 忙等 / 耗时。 */
static void button2_isr_cb(void *ctx, device_event_type_t ev, void *ev_data)
{
    (void)ctx; (void)ev; (void)ev_data;
    rtos_work_submit(&g_btn2_work);     /* ISR 安全：irq_lock 保护链表 + 唤醒 wq */
}

void button_wq_task(void *arg)
{
    app_ctx_t *c = (app_ctx_t *)arg;
    g_btn2_ctx.led = c->led;

    /* 初始化工作节点（fn/arg 只设一次；next 由提交路径维护）。
     * 下半部不建任务 —— 直接复用内核共享 wq worker。 */
    g_btn2_work.fn  = button2_work_fn;
    g_btn2_work.arg = &g_btn2_ctx;
    g_btn2_work.next = (rtos_work_t *)0;

    /* 取按键设备（板子须注册名为 "btn2" 的 exti 设备，见 board.c，PA3/line3） */
    device *btn = device_manager_get("btn2");
    if (!btn) {
        log_printf(app_log(), LOG_INFO, "btn2", "[button2] no 'btn2' device — idle\r\n");
        for (;;) rtos_msleep(1000);
    }

    /* open：经 pinmux 申请引脚、路由 EXTI、注册 ISR、设优先级 */
    btn->vtable->open(btn);
    /* 订阅中断回调（该回调运行在 ISR 上下文 = 上半部） */
    event_device *ev = device_as_event(btn);
    ev->vtable->set_event_callback(ev, DEVICE_EVENT_IRQ, button2_isr_cb, NULL);
    /* 武装 NVIC，等待按键边沿 */
    ev->vtable->enable(ev);

    log_printf(app_log(), LOG_INFO, "btn2", "[button2] ready — BTN2 命令可软件触发边沿(PA3)\r\n");
    /* 任务本体空闲：所有按键工作在【共享 wq 任务】里完成；此处阻塞让出 CPU（不空转）。 */
    for (;;) rtos_msleep(1000);
}

RTOS_TASK(button_wq, "button_wq", button_wq_task, 20, g_button_wq_stack, sizeof(g_button_wq_stack), &g_app_ctx, 1);
