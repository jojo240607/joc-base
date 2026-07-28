/*
 * lock.h 单元自测（host 端：irq_lock/sched_lock 在 host 为空操作桩，
 * 本测试验证接口可用、临界区配对正确、逻辑不被破坏；真实硬件语义由后续
 * 板载 BIST 覆盖）。
 */
#include "common/lock.h"
#include <stdio.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); g_fail++; } \
} while (0)

int main(void) {
    volatile uint32_t counter = 0;

    /* irq_lock / irq_unlock 配对 */
    irq_state_t st = irq_lock();
    counter = 10;
    irq_unlock(st);
    CHECK(counter == 10);
    CHECK(irq_is_disabled() == false);

    /* sched_lock / sched_unlock 配对 */
    sched_lock(4);
    counter += 5;
    sched_unlock();
    CHECK(counter == 15);

    /* 作用域宏：进入后修改、离开后值保持 */
    {
        LOCK_IRQ_SCOPE() {
            counter += 25;
        }
    }
    CHECK(counter == 40);

    if (g_fail == 0) {
        printf("[lock_test] PASS\n");
        return 0;
    }
    printf("[lock_test] FAILED (%d checks)\n", g_fail);
    return 1;
}
