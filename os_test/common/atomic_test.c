/*
 * atomic.h 单元自测（host 端，用 gcc 跑，验证 API 正确性与单线程语义）。
 * 真正的并发正确性由后续板载 BIST（多生产者/ISR 场景）覆盖。
 */
#include "common/atomic.h"
#include <stdio.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); g_fail++; } \
} while (0)

int main(void) {
    atomic_u32_t a;

    /* init / load / store */
    atomic_u32_init(&a, 0);
    CHECK(atomic_u32_load(&a) == 0);
    atomic_u32_store(&a, 42);
    CHECK(atomic_u32_load(&a) == 42);

    /* fetch_add / fetch_sub */
    atomic_u32_init(&a, 10);
    CHECK(atomic_u32_fetch_add(&a, 5) == 10);
    CHECK(atomic_u32_load(&a) == 15);
    CHECK(atomic_u32_fetch_sub(&a, 3) == 15);
    CHECK(atomic_u32_load(&a) == 12);

    /* inc / dec */
    atomic_u32_init(&a, 0);
    CHECK(atomic_u32_inc(&a) == 0);
    CHECK(atomic_u32_inc(&a) == 1);
    CHECK(atomic_u32_load(&a) == 2);
    CHECK(atomic_u32_dec(&a) == 2);
    CHECK(atomic_u32_load(&a) == 1);

    /* fetch_or / fetch_and */
    atomic_u32_init(&a, 0x0F);
    CHECK(atomic_u32_fetch_or(&a, 0x30) == 0x0F);
    CHECK(atomic_u32_load(&a) == 0x3F);
    CHECK(atomic_u32_fetch_and(&a, 0x0F) == 0x3F);
    CHECK(atomic_u32_load(&a) == 0x0F);

    /* swap */
    atomic_u32_init(&a, 100);
    CHECK(atomic_u32_swap(&a, 200) == 100);
    CHECK(atomic_u32_load(&a) == 200);

    /* compare_exchange 成功路径 */
    atomic_u32_init(&a, 7);
    uint32_t exp = 7;
    CHECK(atomic_u32_compare_exchange(&a, &exp, 99) == true);
    CHECK(atomic_u32_load(&a) == 99);

    /* compare_exchange 失败路径：expected 被改写为当前值 */
    exp = 1; /* 与当前值 99 不符 */
    CHECK(atomic_u32_compare_exchange(&a, &exp, 123) == false);
    CHECK(exp == 99);
    CHECK(atomic_u32_load(&a) == 99); /* 未被修改 */

    /* test_and_set / clear bit */
    atomic_u32_init(&a, 0);
    CHECK(atomic_u32_test_and_set_bit(&a, 3) == 0);
    CHECK(atomic_u32_load(&a) == (1u << 3));
    CHECK(atomic_u32_test_and_set_bit(&a, 3) == 1); /* 已置位 */
    CHECK(atomic_u32_test_and_clear_bit(&a, 3) == 1);
    CHECK(atomic_u32_load(&a) == 0);
    CHECK(atomic_u32_test_and_clear_bit(&a, 3) == 0);

    if (g_fail == 0) {
        printf("[atomic_test] PASS\n");
        return 0;
    }
    printf("[atomic_test] FAILED (%d checks)\n", g_fail);
    return 1;
}
