/*
 * pool 单元自测（host 端 gcc 跑，验证 OOC 固定块内存池分配/释放/耗尽/重复释放）。
 * 对应 docs/ostest.md §2.5 内存管理：TC-MEM-001(分配释放) / TC-MEM-002(耗尽返回 NULL)
 * / TC-MEM-003(重复释放不崩) / TC-MEM-004(压力分配释放)。
 *
 * 编译：gcc -I../src common/pool_test.c ../src/common/pool.c -o pool_test
 * （见同目录 run_tests.bat / run_tests.sh / CMakeLists.txt）
 */
#include "common/pool.h"
#include <stdio.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); g_fail++; } \
} while (0)

int main(void) {
    /* TC-MEM-001: 静态池分配/释放，指针有效，释放后可重分配 */
    static uint8_t backing[64 * 8];   /* 8 个 64B 块 */
    pool p;
    pool_init(&p, backing, sizeof(backing), 64);
    CHECK(p.fun->capacity(&p) == 8);
    CHECK(p.fun->empty(&p));

    void *b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = p.fun->alloc(&p);
        CHECK(b[i] != NULL);
        CHECK(p.fun->in_pool(&p, b[i]));
    }
    CHECK(p.fun->full(&p));
    for (int i = 0; i < 8; i++) p.fun->free(&p, b[i]);
    CHECK(p.fun->empty(&p));
    /* 释放后重分配仍能拿到有效块 */
    void *again = p.fun->alloc(&p);
    CHECK(again != NULL);
    p.fun->free(&p, again);

    /* TC-MEM-002: 池耗尽返回 NULL，系统不崩 */
    void *grab[8];
    for (int i = 0; i < 8; i++) grab[i] = p.fun->alloc(&p);
    CHECK(p.fun->full(&p));
    void *over = p.fun->alloc(&p);
    CHECK(over == NULL);              /* 耗尽返回 NULL */
    for (int i = 0; i < 8; i++) p.fun->free(&p, grab[i]);

    /* TC-MEM-003: 重复释放不崩溃（实现上会再次入空闲链；这里只验证不崩且池仍可用） */
    void *x = p.fun->alloc(&p);
    CHECK(x != NULL);
    int r1 = p.fun->free(&p, x) ? 1 : 0;
    int r2 = p.fun->free(&p, x) ? 1 : 0;   /* 重复释放 */
    (void)r1; (void)r2;                     /* 不要求返回错误，仅验证不崩 */
    CHECK(p.fun->available(&p) >= 1);
    /* 重复释放后池仍可分配 */
    void *y = p.fun->alloc(&p);
    CHECK(y != NULL);
    p.fun->free(&p, y);

    /* TC-MEM-004: 随机顺序压力（固定块池，用多池模拟不同大小） */
    pool pa, pb;
    static uint8_t ba[32 * 4];   /* 4 个 32B */
    static uint8_t bb[128 * 2];  /* 2 个 128B */
    pool_init(&pa, ba, sizeof(ba), 32);
    pool_init(&pb, bb, sizeof(bb), 128);
    srand(12345);
    for (int i = 0; i < 1000; i++) {
        if (rand() & 1) {
            void *q = pa.fun->alloc(&pa);
            if (q) pa.fun->free(&pa, q);
        } else {
            void *q = pb.fun->alloc(&pb);
            if (q) pb.fun->free(&pb, q);
        }
    }
    CHECK(p.fun->available(&pa) == pa.fun->capacity(&pa));
    CHECK(p.fun->available(&pb) == pb.fun->capacity(&pb));

    if (g_fail == 0) {
        printf("[pool_test] PASS\n");
        return 0;
    }
    printf("[pool_test] FAILED (%d checks)\n", g_fail);
    return 1;
}
