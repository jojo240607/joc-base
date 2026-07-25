/*
 * name_table.h 单元自测（host 端 gcc 跑，验证名字注册/查找/去重/删除/遍历）。
 */
#include "common/name_table.h"
#include <stdio.h>

typedef struct { int id; } dev_t;

/* 定义一张容量为 4 的 “名字 -> dev_t*” 表 */
NAME_TABLE_DEFINE(devtab, dev_t, 4)

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); g_fail++; } \
} while (0)

int main(void) {
    devtab_t tab;
    devtab_init(&tab);

    dev_t a = {1}, b = {2}, c = {3};

    CHECK(devtab_count(&tab) == 0);
    CHECK(devtab_register(&tab, "uart0", &a) == true);
    CHECK(devtab_register(&tab, "spi1",  &b) == true);
    CHECK(devtab_count(&tab) == 2);

    /* 查找命中 */
    CHECK(devtab_find(&tab, "uart0") == &a);
    CHECK(devtab_find(&tab, "spi1")  == &b);
    /* 查找未命中 */
    CHECK(devtab_find(&tab, "nope")  == (dev_t *)0);

    /* 重名拒绝（返回 false 且不改变计数） */
    CHECK(devtab_register(&tab, "uart0", &c) == false);
    CHECK(devtab_count(&tab) == 2);

    /* 表满：容量 4，再注册两个后满，第 5 个失败 */
    CHECK(devtab_register(&tab, "i2c2", &c) == true);
    CHECK(devtab_is_full(&tab) == false);
    dev_t d = {4};
    CHECK(devtab_register(&tab, "adc3", &d) == true);  /* 第 4 个，满 */
    CHECK(devtab_is_full(&tab) == true);
    dev_t e = {5};
    CHECK(devtab_register(&tab, "extra", &e) == false); /* 溢出拒绝 */

    /* 按索引取 */
    CHECK(devtab_get(&tab, 0) == &a);
    CHECK(devtab_name_of(&tab, 1) != (const char *)0 &&
          strcmp(devtab_name_of(&tab, 1), "spi1") == 0);

    /* 删除 uart0（用末项填补，O(1)），查找应失效，但 spi1 仍在 */
    CHECK(devtab_unregister(&tab, "uart0") == true);
    CHECK(devtab_find(&tab, "uart0") == (dev_t *)0);
    CHECK(devtab_find(&tab, "spi1")  == &b);
    CHECK(devtab_count(&tab) == 3);

    /* 遍历 */
    size_t seen = 0;
    const char *nm;
    dev_t *it;
    NAME_TABLE_FOREACH(devtab, &tab, nm, it) {
        (void)nm; (void)it; seen++;
    }
    CHECK(seen == 3);

    /* clear */
    devtab_clear(&tab);
    CHECK(devtab_count(&tab) == 0);
    CHECK(devtab_find(&tab, "spi1") == (dev_t *)0);

    if (g_fail == 0) {
        printf("[name_table_test] PASS\n");
        return 0;
    }
    printf("[name_table_test] FAILED (%d checks)\n", g_fail);
    return 1;
}
