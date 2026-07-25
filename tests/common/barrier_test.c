/*
 * barrier.h 单元自测。
 *
 * 屏障的正确性（禁止重排）难以在单线程 host 测试里断言，本测试重点验证：
 *   1) 各屏障宏均可展开、可编译（host 与 arm 双目标）；
 *   2) 用 volatile 串起的“写数据->发布->读标志->读数据”序列在运行时得到
 *      预期结果，证明屏障宏不会干扰正常访存顺序。
 */
#include "common/barrier.h"
#include <stdio.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); g_fail++; } \
} while (0)

int main(void) {
    volatile uint32_t data = 0;
    volatile uint32_t ready = 0;
    uint32_t got;

    /* 模拟生产者发布：先写数据，再发布 ready 标志 */
    data = 0x1234U;
    barrier_publish();          /* 确保 data 写入先于 ready */
    ready = 1;
    barrier_compiler();

    /* 模拟消费者获取：先看到 ready，再读数据 */
    if (ready) {
        barrier_acquire();      /* 确保读到 ready 之后才读 data */
        got = data;
    } else {
        got = 0;
    }
    CHECK(got == 0x1234U);

    /* 逐一确认宏都能展开且不报错 */
    barrier_dmb();
    barrier_dsb();
    barrier_isb();
    barrier_read();
    barrier_write();
    barrier_compiler();

    /* 在循环里使用屏障，验证不会被优化成死代码 */
    volatile uint32_t acc = 0;
    for (int i = 0; i < 4; i++) {
        acc += (uint32_t)i;
        barrier_dmb();
    }
    CHECK(acc == 6U); /* 0+1+2+3 */

    if (g_fail == 0) {
        printf("[barrier_test] PASS\n");
        return 0;
    }
    printf("[barrier_test] FAILED (%d checks)\n", g_fail);
    return 1;
}
