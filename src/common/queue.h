#ifndef JOC_BASE_DS_QUEUE_H
#define JOC_BASE_DS_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 不透明静态数组循环队列对象
 *
 * 与 ring（字节流）的区别：本队列以“固定大小的元素”为单位存取，
 * 适合存放整数、指针或小型结构体。元素大小在初始化时由调用方指定，
 * 后备缓冲区被切分为 N 个等长的元素槽，元素通过 memcpy 进出队列。
 *
 *  - 静态分配：调用方提供后备缓冲区（RAM 数组），由 queue_init 绑定；
 *  - 堆分配：  用 queue_create 让对象自行为缓冲区与对象 malloc（MCU 上可选）。
 * 两种用法均通过同一套 queue 对象接口操作，行为可被派生类型覆写（多态）。
 */
typedef struct queue queue;

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * 循环队列的全部能力都在虚表里，queue 成为可派生的对象。
 * 所有公开方法只能通过 self->fun->method(self, ...) 分派。
 * ------------------------------------------------------------------------- */
struct queueFun {
    /* 入队：拷贝一个元素到队尾，满则返回 false */
    bool (*enqueue)(queue *self, const void *item);
    /* 出队：拷贝队首元素到 item 并移除，空则返回 false */
    bool (*dequeue)(queue *self, void *item);
    /* 窥视队首：拷贝但不移除，空则返回 false */
    bool (*peek)(const queue *self, void *item);

    /* 容量与计数 */
    size_t (*capacity)(const queue *self);  /* 元素容量（槽位数） */
    size_t (*size)(const queue *self);      /* 已存储元素数 */
    size_t (*available)(const queue *self); /* 剩余可入队元素数 */
    bool (*empty)(const queue *self);
    bool (*full)(const queue *self);

    /* 清空（仅复位指针与计数） */
    void (*clear)(queue *self);

    /* 生命周期（对应 init / create） */
    void (*deinit)(queue *self);
    void (*destroy)(queue *self);
};

/* 循环队列对象定义（结构体完整可见，便于静态/栈分配） */
struct queue {
    const struct queueFun *fun;
    uint8_t *buf;        /* 对齐后的后备缓冲区起点（元素槽从这里排布） */
    size_t elem_size;    /* 用户元素大小（字节） */
    size_t stride;       /* 实际槽位步长（含对齐填充，且 >= sizeof(void*)） */
    size_t capacity;     /* 元素容量（槽位数） */
    size_t head;         /* 读索引：下一次出队位置 */
    size_t tail;         /* 写索引：下一次入队位置 */
    size_t count;        /* 已存储元素数（避免 head==tail 的满/空歧义） */
    bool owns;           /* 后备缓冲区是否由本对象堆分配（deinit/destroy 时释放） */
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
queue *queue_create(size_t elem_size, size_t capacity);                       /* 堆分配后备缓冲区 + 初始化 */
void   queue_init(queue *self, void *buf, size_t buf_size, size_t elem_size); /* 绑定用户提供的静态缓冲区 */
void   queue_deinit(queue *self);                                             /* 对应 init，释放对象侧资源 */
void   queue_destroy(queue *self);                                            /* 对应 create，释放堆内存 */

/* 方法表实例（由 queue.c 提供并赋值给 self->fun） */
extern const struct queueFun queue_fun;

#endif /* JOC_BASE_DS_QUEUE_H */
