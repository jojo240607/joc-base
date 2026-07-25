#ifndef JOC_BASE_DS_RING_H
#define JOC_BASE_DS_RING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 不透明环形缓冲区对象（用于字节流存储）
 *
 * 设计目标：嵌入式场景下的“静态”环形缓冲区。
 *  - 静态分配：调用方提供后备缓冲区（RAM 数组），用 ring_init 绑定；
 *  - 堆分配：  用 ring_create 让对象自行为缓冲区 malloc（MCU 上可选）。
 * 两种用法均通过同一套 ring 对象接口操作，行为可被派生类型覆写（多态）。
 */
typedef struct ring ring;

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * 环形缓冲区的全部能力都在虚表里，ring 成为可派生的对象。
 * 所有公开方法只能通过 self->fun->method(self, ...) 分派。
 * ------------------------------------------------------------------------- */
struct ringFun {
    /* 判空 / 判满 */
    bool (*empty)(const ring *self);
    bool (*full)(const ring *self);

    /* 容量与计数 */
    size_t (*capacity)(const ring *self);  /* 缓冲区总容量（字节） */
    size_t (*size)(const ring *self);      /* 已存储字节数 */
    size_t (*space)(const ring *self);     /* 剩余可写字节数 */

    /* 单字节读写 */
    bool (*push)(ring *self, uint8_t byte);          /* 写一字节，满则返回 false */
    bool (*pop)(ring *self, uint8_t *byte);          /* 读一字节，空则返回 false */
    bool (*peek)(const ring *self, uint8_t *byte);   /* 窥视队首，不取出 */

    /* 批量读写（返回实际读/写字节数，受剩余空间/数据量限制）*/
    size_t (*write)(ring *self, const void *src, size_t len);
    size_t (*read)(ring *self, void *dst, size_t len);

    /* 清空（仅复位指针与计数，不触碰后备缓冲区内容）*/
    void (*clear)(ring *self);

    /* 生命周期（对应 init / create） */
    void (*deinit)(ring *self);
    void (*destroy)(ring *self);
};

/* 环形缓冲区对象定义（结构体完整可见，便于静态/栈分配） */
struct ring {
    const struct ringFun *fun;
    uint8_t *buf;        /* 后备缓冲区：静态数组或堆分配 */
    size_t capacity;     /* 缓冲区大小（字节） */
    size_t head;         /* 写指针：下一次写入位置 */
    size_t tail;         /* 读指针：下一次读出位置 */
    size_t count;        /* 已存储字节数（避免 head/tail 相等时的满/空歧义） */
    bool owns;           /* 缓冲区是否由本对象堆分配（deinit/destroy 时释放） */
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
ring *ring_create(size_t capacity);                       /* 堆分配缓冲区 + 初始化 */
void  ring_init(ring *self, uint8_t *buf, size_t capacity); /* 绑定用户提供的静态缓冲区 */
void  ring_deinit(ring *self);                            /* 对应 init，释放对象侧资源 */
void  ring_destroy(ring *self);                           /* 对应 create，释放堆内存 */

/* 方法表实例（由 ring.c 提供并赋值给 self->fun） */
extern const struct ringFun ring_fun;

#endif /* JOC_BASE_DS_RING_H */
