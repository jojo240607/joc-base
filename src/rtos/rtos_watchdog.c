#include "rtos.h"
#include "core/rtos_internal.h"   /* g_tick / rtos_timer_tick / rtos_timer_run_pending / rtos_crit_enter/exit */
#include "system_init.h"          /* reset_reason_t / board_decode_reset_reason（自测验证解码逻辑） */
#include "devmgr/device_manager.h"
#include "drv/iwdg.h"
#include <stdint.h>
#include <stddef.h>

/* ===========================================================================
 * 看门狗喂狗 + 马拉松长跑（rtos-test-plan §6.4，准则 rtos-test.md §4）
 *
 * - 软件定时器原语(§6.3) 正好用来驱动“周期喂狗”：把 IWDG 的刷新注册为一个
 *   周期 rtos_timer，回调在【定时器任务】上下文执行（特权态，可写 KR 备份域）。
 *   若系统在某高优先级任务里死锁/忙等，定时器任务抢不到 CPU，IWDG 超时复位——
 *   这正是看门狗的意义。
 * - IWDG 一旦 START(KR=0xCCCC) 就存活至下次复位（备份域），故 rtos_watchdog_enable
 *   是【致命】操作，仅在马拉松模式由控制台显式触发；自测绝不调用它（只验证喂狗路径）。
 * - 复位原因由 board_decode_reset_reason / board_report_reset_reason 在 BIST 开头
 *   打印（见 system_init.c），马拉松看门狗复位后会再次打印 "IWDG/WWDG"。
 * ======================================================================== */

/* IWDG 时间基准（LSI 名义 32 kHz；STM32F4 典型 32 kHz，范围 30–60 kHz）。
 * 仅用于把 timeout_ms 映射到 PR/RLR；实测偏差由马拉松长跑暴露。 */
#ifndef IWDG_LSI_HZ
  #define IWDG_LSI_HZ 32000U
#endif

static device        *g_wdt_dev;
static rtos_timer_t   g_wdt_timer;
static uint32_t       g_wdt_feeds;   /* 喂狗次数（诊断 + 自测） */
static uint8_t        g_wdt_armed;   /* 1 = IWDG 已 START（存活至下次复位） */
static uint8_t        g_wdt_feed_on; /* 1 = 周期喂狗定时器已注册 */

/* 把超时(ms)映射到 IWDG 预分频码 pr(0..6) 与重装值 rlr(0..4095)。
 * 预分频 div = 4 << pr；rlr = timeout_ms/1000 * LSI / div - 1，取能装下的最小 pr。 */
void rtos_watchdog_compute(uint32_t timeout_ms, uint32_t *pr, uint32_t *rlr)
{
    uint32_t t = timeout_ms ? timeout_ms : 1U;
    for (uint32_t p = 0; p <= 6; p++) {
        uint64_t div = (uint64_t)4U << p;                 /* 4,8,16,...,256 */
        uint64_t ticks = (uint64_t)t * IWDG_LSI_HZ / 1000U / div;
        if (ticks == 0) ticks = 1;
        if (ticks <= 0x1000ULL) {                         /* 留 1 余量，rlr 最大 0xFFF */
            *pr = p;
            *rlr = (uint32_t)(ticks - 1U);
            return;
        }
    }
    *pr = 6; *rlr = 0xFFF;                                /* 装不下：最长超时 */
}

/* 周期喂狗回调（定时器任务上下文，特权态）：刷新 IWDG 下计数器。 */
static void wdt_feed_cb(rtos_timer_t *t, void *arg)
{
    (void)t; (void)arg;
    if (g_wdt_dev) {
        g_wdt_dev->vtable->ioctl(g_wdt_dev, IWDG_IOCTL_REFRESH, NULL);
        g_wdt_feeds++;
    }
}

void rtos_watchdog_feed(void)
{
    if (g_wdt_dev)
        g_wdt_dev->vtable->ioctl(g_wdt_dev, IWDG_IOCTL_REFRESH, NULL);
    g_wdt_feeds++;   /* 手动喂狗计数（即便设备缺失也记录调用，便于诊断） */
}

/* 仅注册周期喂狗定时器（不 arming）。供自测确定性驱动，也供“软看门狗”模式使用。 */
static int wdt_setup_feed(uint32_t feed_ms)
{
    if (!g_wdt_dev) g_wdt_dev = device_manager_get("iwdg0");
    if (!g_wdt_dev) return -1;
    if (g_wdt_feed_on) return 0;
    uint32_t period_t = feed_ms * RTOS_TICK_HZ / 1000U;
    if (period_t == 0) period_t = 1;
    rtos_timer_init(&g_wdt_timer, "wdt", wdt_feed_cb, NULL);
    rtos_timer_start_ticks(&g_wdt_timer, RTOS_TIMER_PERIODIC, period_t);
    g_wdt_feed_on = 1;
    return 0;
}

/* 启动看门狗：配置 PR/RLR 并 ARM（IWDG 存活至下次复位！仅在马拉松模式显式调用）。
 * 喂狗周期为 timeout 的一半，留出充足余量。返回 0 成功 / -1 无设备。 */
int rtos_watchdog_enable(uint32_t timeout_ms)
{
    if (!g_wdt_dev) g_wdt_dev = device_manager_get("iwdg0");
    if (!g_wdt_dev) return -1;
    if (g_wdt_dev->vtable->open(g_wdt_dev) != 0) return -1;
    uint32_t pr = 0, rlr = 0;
    rtos_watchdog_compute(timeout_ms, &pr, &rlr);
    g_wdt_dev->vtable->ioctl(g_wdt_dev, IWDG_IOCTL_SET_PRESCALER, &pr);
    g_wdt_dev->vtable->ioctl(g_wdt_dev, IWDG_IOCTL_SET_RELOAD,    &rlr);
    wdt_setup_feed(timeout_ms / 2);
    g_wdt_dev->vtable->ioctl(g_wdt_dev, IWDG_IOCTL_START, NULL);  /* 致命：armed */
    g_wdt_armed = 1;
    return 0;
}

int  rtos_watchdog_is_armed(void) { return g_wdt_armed; }
uint32_t rtos_watchdog_feeds(void) { return g_wdt_feeds; }

/* ---- 马拉松长跑任务组（准则 §4：同时跑多个任务至少 72h） ----
 * 这里提供“长跑任务组”机制；72h 是让它一直跑（交互命令 RTOSMARATHON 触发）。
 * 心跳任务只累加各自计数器并 msleep——开销极小；若系统挂死，WDT（可选）会复位。
 *
 * 重启安全（修复双 RTOSMARATHON 死机 INVSTATE + 死锁）：
 *   旧实现用单个共享标志 g_marathon_stop + 同一组静态栈。重启时先 stop(置 1, 阻塞等
 *   信号量) 再 start(把标志【复位为 0】并在【同一块栈】上建新任务)。若上一轮某个
 *   worker 还没从 rtos_msleep(50) 醒来，它读到已被复位的 0 误以为“没让停”，便一直跑，
 *   与新一轮同栈 worker 成为【两个同时存活、共用 512B 栈】的任务，互相踩踏保存帧
 *   (EXC_RETURN/xPSR) -> bx EXC_RETURN 读到 0 -> INVSTATE；另外若恰好有 worker 漏给
 *   信号量，stop 的 rtos_sem_wait 会永久阻塞控制台 -> 整机无响应。
 *   新设计（非阻塞、零共享）：
 *   1) 代际令牌 g_marathon_gen：worker 只在“本代令牌不变”时跑。start 每次【自增令牌】
 *      （绝不复位成“继续跑”的值），任何迟到唤醒的旧 worker 因令牌变了必然退出，不会
 *      赖在栈上。start 不再调用阻塞式 stop——旧 worker 在 ~50ms 内自行退出，不阻塞控制台。
 *   2) 双缓冲栈 g_marathon_stack_arr[2][N]（ping-pong）：start 每次把缓冲索引翻转，新代
 *      永远用【另一块】内存。即便交接窗口内有旧 worker 尚存活，它也只在旧缓冲上运行，
 *      与新代物理隔离——绝不会两个活任务共用同一块栈。旧代退出后其缓冲才在下一轮被复用。 */
#define MARATHON_N 3
#define MARATHON_STACK_SZ 512
#define MARATHON_GENS 2
static uint8_t  g_marathon_on;
static volatile uint8_t  g_marathon_gen;     /* 代际令牌：worker 仅在令牌匹配时运行 */
static int                g_marathon_buf;    /* ping-pong 缓冲索引(0/1)，每次 start 翻转 */
static volatile uint32_t g_marathon_beat[MARATHON_N];
static rtos_sem_t        g_marathon_done;
/* 马拉松工作栈放在【主 SRAM】(而非 CCM(.ccm_bss))：CCM 仅 63K，已承载 MSP(顶 1K)
 * + TCB 池(g_task_pool[48]) + 常驻任务栈(main/blink/idle/bist/wq/bh/各 selftest)，
 * 空间紧张。开发者已把 rtos_ostest/robust/basic 的自测栈挪到主 SRAM，明确注释
 * "避免 CCM 与 MSP/TCB 池争用导致溢出相互踩踏"。双缓冲仍保持 2 的幂大小 + 基址对齐，
 * 满足 MPU 每任务栈 region(R4)。 */
static uint8_t g_marathon_stack_arr[MARATHON_GENS][MARATHON_N][MARATHON_STACK_SZ]
    __attribute__((aligned(MARATHON_STACK_SZ)));

static void marathon_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    uint8_t my_gen = g_marathon_gen;        /* 捕获本代令牌；gen 自增后必退出 */
    while (g_marathon_gen == my_gen) {
        g_marathon_beat[id]++;
        rtos_msleep(50);
    }
    rtos_sem_give(&g_marathon_done);        /* 无害：无人等待时计数封顶，仅作退出标记 */
}

/* 启动长跑：派生 N 个心跳任务（不同优先级）常驻；arm_wdt!=0 时同时 ARM 看门狗。
 * 返回 0 成功。重复调用 = 重启：自增令牌让旧 worker 自行退出，新代用另一块缓冲，
 * 全程非阻塞、不共享栈，绝不会死机或死锁。 */
int rtos_marathon_start(uint8_t arm_wdt)
{
    g_marathon_gen++;                                   /* 令牌自增：旧 worker 见此必退出 */
    rtos_sem_init(&g_marathon_done, 0, MARATHON_N + 1);
    g_marathon_buf ^= 1;                                /* ping-pong 翻转：本代用另一块缓冲 */
    int prio[MARATHON_N] = { 20, 22, 24 };              /* 均低于主任务(16)，后台心跳 */
    for (int i = 0; i < MARATHON_N; i++) {
        rtos_task_create("marathon", marathon_worker, (void *)(intptr_t)i,
                         (uint8_t)prio[i],
                         g_marathon_stack_arr[g_marathon_buf][i],
                         sizeof(g_marathon_stack_arr[g_marathon_buf][i]));
    }
    g_marathon_on = 1;
    if (arm_wdt) rtos_watchdog_enable(2000); /* 2s 超时 / 1s 喂：马拉松模式才 arming */
    return 0;
}

/* 停止长跑（非阻塞）：自增令牌，所有当前 worker 在 ~50ms 内自行退出并释放其缓冲；
 * 不阻塞等待，故任何上下文调用都安全，不会死锁控制台。 */
void rtos_marathon_stop(void)
{
    g_marathon_gen++;                                   /* 信号所有当前 worker 停止(含迟到唤醒者) */
    g_marathon_on = 0;
}

int rtos_marathon_is_running(void) { return g_marathon_on; }

#if RTOS_SELFTEST
/* ---- 自测（RTOSALL "watchdog" 条目）：仅验证安全、可确定性判定的部分 ----
 * 1) 复位原因解码（纯函数，跨各种 CSR 位组合，含优先级）；
 * 2) 喂狗路径（rtos_watchdog_feed 计数 +1，不 arming，安全）；
 * 3) 周期喂狗定时器集成（不 arming；irq_lock 窗口内手动推进 g_tick 驱动回调）。 */
int rtos_watchdog_selftest(void)
{
    int ok = 1;

    /* 1) 复位原因解码：STM32F4 上 IWDG/WWDG 同位，故看门狗统一为 IWDG。
     *    用与 device/stm32f407xx.h 一致的位号（避免 rtos 层直接 include 芯片头）。 */
    struct { uint32_t csr; reset_reason_t exp; } cases[] = {
        { (1u<<29), RESET_REASON_IWDG },                       /* IWDGRSTF */
        { (1u<<30), RESET_REASON_IWDG },                       /* WWDGRSTF(=IWDG 别名) */
        { (1u<<28), RESET_REASON_SOFTWARE },                   /* SFTRSTF */
        { (1u<<27), RESET_REASON_POWER },                      /* PORRSTF */
        { (1u<<26), RESET_REASON_PIN },                        /* PINRSTF */
        { (1u<<31), RESET_REASON_LOWPWR },                     /* LPWRRSTF */
        { (1u<<26)|(1u<<27), RESET_REASON_POWER },             /* POR>PIN 优先级 */
        { (1u<<29)|(1u<<28), RESET_REASON_IWDG },              /* IWDG>SFT 优先级 */
        { 0,        RESET_REASON_UNKNOWN },
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        reset_reason_t r = board_decode_reset_reason(cases[i].csr);
        if (r != cases[i].exp) { ok = 0; break; }
    }

    /* 2) 设备存在 + 喂狗路径（不 arming，安全） */
    device *d = device_manager_get("iwdg0");
    if (!d) { ok = 0; }
    else {
        g_wdt_dev = d;
        uint32_t before = g_wdt_feeds;
        rtos_watchdog_feed();
        if (g_wdt_feeds != before + 1) ok = 0;
    }

    /* 3) 周期喂狗定时器集成（不 arming）：irq_lock 窗口内手动越过一个周期，
     *    调用 rtos_timer_tick + rtos_timer_run_pending 执行回调（写 KR=0xAAAA，无害）。 */
    if (d) {
        rtos_timer_t t;
        rtos_timer_init(&t, "wdt_probe", wdt_feed_cb, NULL);
        rtos_timer_start_ticks(&t, RTOS_TIMER_PERIODIC, 10);   /* 10 tick 周期 */
        unsigned st = rtos_crit_enter();
        uint32_t before = g_wdt_feeds;
        g_tick = (uint32_t)(g_tick + 10);                      /* 手动越过一个周期 */
        rtos_timer_tick();                                     /* 置 pending + 唤醒定时器任务 */
        rtos_timer_run_pending();                              /* 在当前上下文执行回调(喂狗) */
        rtos_crit_exit(st);
        rtos_timer_stop(&t);
        if (g_wdt_feeds <= before) ok = 0;                     /* 回调应至少触发一次 */
    }
    return ok;
}
RTOS_SELFTEST_ADD("watchdog", rtos_watchdog_selftest);
#endif /* RTOS_SELFTEST */
