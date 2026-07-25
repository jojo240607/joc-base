#ifndef JOC_BASE_DS_PQUEUE_H
#define JOC_BASE_DS_PQUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 不透明静态数组最小堆优先队列
 *
 *  - 元素为 void*（可直接存用户定义结构体指针，比较时由回调解引用）；
 *  - 优先级通过函数指针 cmp 比较：cmp(a, b) < 0 表示 a 优先级更高
 *    （即 a 在堆序上“更小”，应更靠近堆顶），因此堆顶始终是 cmp 意义下的最小元素；
 *  - 后备缓冲区是一段 void* 数组，由调用方提供（静态/栈分配）或由 pqueue_create 堆分配；
 *  - 底层为二叉最小堆，push/pop 均为 O(log n)。
 *
 * 两种用法均通过同一套 pqueue 对象接口操作，行为可被派生类型覆写（多态）。
 */
typedef struct pqueue pqueue;

/* 优先级比较回调：a、b 为队列中存储的 void* 元素值（即用户指针）。
 * 返回 <0 / 0 / >0 分别表示 a 优先级高于 / 等于 / 低于 b。 */
typedef int (*pqueue_cmp)(const void *a, const void *b);

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * 优先队列的全部能力都在虚表里，pqueue 成为可派生的对象。
 * 所有公开方法只能通过 self->fun->method(self, ...) 分派。
 * ------------------------------------------------------------------------- */
struct pqueueFun {
    /* 入队：将元素指针加入堆并 sift-up，满则返回 false */
    bool (*push)(pqueue *self, void *item);
    /* 出队：取走堆顶（最小）元素，sift-down 重排；空则返回 false */
    bool (*pop)(pqueue *self, void **item);
    /* 窥视堆顶：通过 out 参数返回，不移除；空则返回 false */
    bool (*peek)(const pqueue *self, void **item);

    /* 容量与计数 */
    size_t (*capacity)(const pqueue *self);  /* 元素容量（槽位数） */
    size_t (*size)(const pqueue *self);      /* 已存储元素数 */
    bool (*empty)(const pqueue *self);
    bool (*full)(const pqueue *self);

    /* 清空（仅复位计数，不释放/触碰缓冲区内容）*/
    void (*clear)(pqueue *self);

    /* 生命周期（对应 init / create） */
    void (*deinit)(pqueue *self);
    void (*destroy)(pqueue *self);
};

/* 优先队列对象定义（结构体完整可见，便于静态/栈分配） */
struct pqueue {
    const struct pqueueFun *fun;
    void **buf;                 /* 对齐后的 void* 元素数组（堆的存储） */
    size_t capacity;            /* 元素容量（槽位数） */
    size_t count;               /* 已存储元素数 */
    pqueue_cmp cmp;             /* 优先级比较回调 */
    bool owns;                  /* 后备缓冲区是否由本对象堆分配（deinit/destroy 时释放） */
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
pqueue *pqueue_create(size_t capacity, pqueue_cmp cmp);                          /* 堆分配后备缓冲区 + 初始化 */
void    pqueue_init(pqueue *self, void *buf, size_t buf_size, pqueue_cmp cmp);   /* 绑定用户提供的 void* 数组 */
void    pqueue_deinit(pqueue *self);                                             /* 对应 init，释放对象侧资源 */
void    pqueue_destroy(pqueue *self);                                            /* 对应 create，释放堆内存 */

/* 方法表实例（由 pqueue.c 提供并赋值给 self->fun） */
extern const struct pqueueFun pqueue_fun;

#endif /* JOC_BASE_DS_PQUEUE_H */
