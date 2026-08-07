/**
 * 用户层演示 / 测试任务集合（干净默认配置下唯一的应用任务）。
 *
 * 这些任务全部经 RTOS_TASK 段在 rtos_start() 自动实例化；另提供一个
 * 由控制台 DEMO 命令运行时动态创建的任务，演示用户层可在运行时建任务。
 *
 * 栈用 RTOS_TASK_STACK 声明（2 的幂 + 对齐，启用 MPU 每任务栈 region）。
 */
#include "task_demo.h"
#include "app_shared.h"
#include "console.h"
#include "rtos.h"
#include "iface/device.h"
#include "drv/gpio_pin.h"
#include "log/log.h"
#include "log/app_log.h"
#include <stdio.h>

/* 跨任务 IPC：非特权任务等待该信号量，hello 任务周期性释放，证明 SVC 门可用。
 * 放在本文件静态区即可（demo 内部使用）。 */
static rtos_sem_t g_demo_sem;

/* ---- 任务 1：特权周期任务，翻转 LED + 心跳（取代原 blink） ---- */
RTOS_TASK_STACK(g_demo_led_stack, 1024);

void demo_led_task(void *arg)
{
    app_ctx_t *c = (app_ctx_t *)arg;
    while (!*c->demo_ready) rtos_msleep(10);
    for (;;) {
        if (c->led) c->led->vtable->ioctl(c->led, GPIO_IOCTL_TOGGLE, (void *)0);
        (*c->heartbeat)++;
        rtos_msleep(500);
    }
}
RTOS_TASK(demo_led, "demo_led", demo_led_task, RTOS_PRIO_BLINK,
          g_demo_led_stack, sizeof(g_demo_led_stack), &g_app_ctx, 1);

/* ---- 任务 2：特权周期任务，周期性 log（演示 msleep 阻塞 + 多任务并发） ---- */
RTOS_TASK_STACK(g_demo_hello_stack, 1024);

void demo_hello_task(void *arg)
{
    (void)arg;
    static uint32_t s_cnt = 0;
    rtos_sem_init(&g_demo_sem, 0, 16);   /* 初值 0：非特权任务会在此阻塞直到被释放 */
    for (;;) {
        s_cnt++;
        log_printf(app_log(), LOG_INFO, "demo",
                   "[demo_hello] tick=%lu cnt=%lu — releasing sem for UNPRIV user task\n",
                   (unsigned long)rtos_tick_count(), (unsigned long)s_cnt);
        rtos_sem_give(&g_demo_sem);          /* 唤醒等待的非特权任务 */
        rtos_msleep(1000);
    }
}
RTOS_TASK(demo_hello, "demo_hello", demo_hello_task, RTOS_PRIO_MAIN - 1,
          g_demo_hello_stack, sizeof(g_demo_hello_stack), (void *)0, 1);

/* ---- 任务 3：非特权用户态任务（priv=0），经 SVC 门做 IPC ---- */
RTOS_TASK_STACK(g_demo_user_stack, 1024);

void demo_user_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 非特权任务调用 IPC 原语 -> 经 SVC 门进入特权 Handler 模式执行。
         * 这里会真实阻塞（pend），由 demo_hello 周期 give 唤醒。
         * 注意：非特权任务【不能】直接碰外设（如 UART），也不能写 main-SRAM
         * 的 .bss/.data（unpriv 只开了 CCM 区），否则触发 MemFault；
         * 故此处只做 SVC 门 IPC，不调用 log_printf、不碰任何全局，唤醒由
         * 特权 hello 任务的 give 计数间接证明 SVC 门工作正常。 */
        rtos_sem_wait(&g_demo_sem);
    }
}
RTOS_TASK(demo_user, "demo_user", demo_user_task, RTOS_PRIO_MAIN - 2,
          g_demo_user_stack, sizeof(g_demo_user_stack), (void *)0, 0);

/* ---- 任务 4：由 DEMO 控制台命令动态创建（证明运行时用户层建任务） ---- */
RTOS_TASK_STACK(g_demo_dynamic_stack, 1024);
static uint32_t s_dynamic_spawn = 0;

void demo_dynamic_task(void *arg)
{
    (void)arg;
    uint32_t id = ++s_dynamic_spawn;
    for (int i = 0; i < 5; i++) {
        log_printf(app_log(), LOG_INFO, "demo",
                   "[demo_dynamic #%lu] run %d/5 — created at runtime via rtos_task_create\n",
                   (unsigned long)id, i + 1);
        rtos_msleep(300);
    }
    log_printf(app_log(), LOG_INFO, "demo",
               "[demo_dynamic #%lu] done — returning (slot reusable)\n", (unsigned long)id);
    /* 直接 return：任务返回地址被框架置为 rtos_task_exit，会自动置 DEAD 释放槽位 */
}

void cmd_demo(app_ctx_t *c, const char *line)
{
    (void)line;
    /* rtos_task_create 返回 void：它直接把任务挂入就绪队列；创建无条件成功（槽有限，满则静默丢弃）。
     * 这里只负责“从用户层在运行时建一个任务”。 */
    rtos_task_create("demo_dyn",
                     demo_dynamic_task, (void *)0,
                     RTOS_PRIO_MAIN - 3,
                     g_demo_dynamic_stack, sizeof(g_demo_dynamic_stack));
    char out[48];
    int n = snprintf(out, sizeof(out), "DEMO: spawned dynamic task (5x300ms)\r\n");
    c->console->vtable->write(c->console, out, (size_t)n);
}
