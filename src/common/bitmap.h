#ifndef JOC_BASE_DS_BITMAP_H
#define JOC_BASE_DS_BITMAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * 静态位图（Bitmap）：高效管理大量布尔标志
 *
 * 设计目标：用位（1 bit = 1 个标志）紧凑地表示大量布尔状态，典型用于
 *   - 资源/对象占用标记（配合 objpool 使用）
 *   - 事件/通道就绪标志
 *   - 任何“开/关”状态集合
 *
 *  - 静态分配：调用方提供后备字节数组，由 bitmap_init 绑定（owns == false）；
 *  - 堆分配：  bitmap_create 让对象自行为“位数/字节”分配缓冲区（owns == true）。
 * 两种用法都经由同一套 bitmap 对象接口（虚表）操作，可被派生类型覆写（多态）。
 *
 * 约定：
 *   - 位编号从 0 开始（bit 0 是 bits[0] 的最低位）；
 *   - 当管理的位数不是 8 的整数倍时，最后一个字节中“超出位数”的高位始终为 0，
 *     不参与计数、查找与 any/all 判定（不会被误认为已设置）。
 * ------------------------------------------------------------------------- */
typedef struct bitmap bitmap;

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * ------------------------------------------------------------------------- */
struct bitmapFun {
    /* 单位点操作：成功返回 true，越界返回 false（不改变位）*/
    bool (*set)(bitmap *self, size_t index);          /* 置 1 */
    bool (*reset)(bitmap *self, size_t index);        /* 清 0（单点）*/
    bool (*toggle)(bitmap *self, size_t index);       /* 翻转 */
    bool (*test)(const bitmap *self, size_t index);   /* 读位：1 返回 true，0/越界返回 false */

    /* 批量操作 */
    void (*set_all)(bitmap *self);    /* 全部置 1（仅置有效位）*/
    void (*clear)(bitmap *self);      /* 全部清 0 */

    /* 统计与查询 */
    size_t (*capacity)(const bitmap *self);  /* 可管理的位数 */
    size_t (*count_set)(const bitmap *self); /* 已置 1 的位数（population count）*/
    bool (*any_set)(const bitmap *self);     /* 至少有一个 1 */
    bool (*none_set)(const bitmap *self);    /* 全为 0 */
    bool (*all_set)(const bitmap *self);     /* 全为 1（有效位）*/

    /* 扫描：找到第一个置位/清零的位，写入 *index，成功返回 true；无则返回 false */
    bool (*find_first_set)(const bitmap *self, size_t *index);
    bool (*find_first_clear)(const bitmap *self, size_t *index);

    /* 生命周期（对应 init / create）*/
    void (*deinit)(bitmap *self);
    void (*destroy)(bitmap *self);
};

/* 位图对象定义（结构体完整可见，便于静态/栈分配） */
struct bitmap {
    const struct bitmapFun *fun;
    uint8_t *bits;     /* 对齐后的后备字节数组 */
    size_t  num_bits;  /* 可管理的位数（<= bytes*8）*/
    bool    owns;      /* 后备缓冲区是否由本对象堆分配（deinit/destroy 时释放） */
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
bitmap *bitmap_create(size_t num_bits);                                /* 堆分配缓冲区 + 初始化 */
void    bitmap_init(bitmap *self, void *buf, size_t buf_size);         /* 绑定用户字节数组：位数 = buf_size*8 */
void    bitmap_init_n(bitmap *self, void *buf, size_t buf_size, size_t num_bits); /* 绑定并限制位数 */
void    bitmap_deinit(bitmap *self);                                   /* 对应 init，释放对象侧资源 */
void    bitmap_destroy(bitmap *self);                                  /* 对应 create，释放堆内存 */

/* 方法表实例（由 bitmap.c 提供并赋值给 self->fun） */
extern const struct bitmapFun bitmap_fun;

#endif /* JOC_BASE_DS_BITMAP_H */
