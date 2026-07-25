#include "queue.h"

#include <stdlib.h>
#include <string.h>

/* 槽位对齐粒度：至少要能容下一个 void*，保证存入指针/整数时地址对齐安全 */
#define QUEUE_ALIGN (sizeof(void *))

/* 向上取整到 align 的整数倍 */
#define QUEUE_ROUND_UP(n, a) (((n) + (a) - 1) / (a) * (a))

/* ---------------------------------------------------------------------------
 * vtable 方法实现：全部行为集中在虚表里，queue 成为真正的对象
 * ------------------------------------------------------------------------- */

static bool queue_method_enqueue(queue *self, const void *item) {
    if (self->count == self->capacity) return false;   /* 满 */
    memcpy(self->buf + self->tail * self->stride, item, self->elem_size);
    self->tail = (self->tail + 1U) % self->capacity;
    self->count++;
    return true;
}

static bool queue_method_dequeue(queue *self, void *item) {
    if (self->count == 0) return false;                /* 空 */
    if (item) memcpy(item, self->buf + self->head * self->stride, self->elem_size);
    self->head = (self->head + 1U) % self->capacity;
    self->count--;
    return true;
}

static bool queue_method_peek(const queue *self, void *item) {
    if (self->count == 0) return false;                /* 空 */
    if (item) memcpy(item, self->buf + self->head * self->stride, self->elem_size);
    return true;
}

static size_t queue_method_capacity(const queue *self) {
    return self->capacity;
}

static size_t queue_method_size(const queue *self) {
    return self->count;
}

static size_t queue_method_available(const queue *self) {
    return self->capacity - self->count;
}

static bool queue_method_empty(const queue *self) {
    return self->count == 0;
}

static bool queue_method_full(const queue *self) {
    return self->count == self->capacity;
}

static void queue_method_clear(queue *self) {
    self->head  = 0;
    self->tail  = 0;
    self->count = 0;
}

static void queue_method_deinit(queue *self) {
    /* 缓冲区由对象 heap 分配时才释放；用户静态缓冲区（owns==false）不动 */
    if (self->owns && self->buf) {
        free(self->buf);
        self->buf = NULL;
    }
    self->capacity = 0;
}

static void queue_method_destroy(queue *self) {
    if (!self) return;
    self->fun->deinit(self);   /* 先释放后备缓冲区（若 owns） */
    free(self);
}

/* ---------------------------------------------------------------------------
 * 方法表实例
 * ------------------------------------------------------------------------- */
const struct queueFun queue_fun = {
    .enqueue   = queue_method_enqueue,
    .dequeue   = queue_method_dequeue,
    .peek      = queue_method_peek,
    .capacity  = queue_method_capacity,
    .size      = queue_method_size,
    .available = queue_method_available,
    .empty     = queue_method_empty,
    .full      = queue_method_full,
    .clear     = queue_method_clear,
    .deinit    = queue_method_deinit,
    .destroy   = queue_method_destroy,
};

/* ---------------------------------------------------------------------------
 * 构造函数 / 工厂（自由函数，符合 OOC 惯例）
 * ------------------------------------------------------------------------- */

/* 堆分配：对象本身 + 后备缓冲区均由 malloc 获得（owns == true） */
queue *queue_create(size_t elem_size, size_t capacity) {
    if (elem_size == 0 || capacity == 0) return NULL;

    size_t stride = QUEUE_ROUND_UP(elem_size, QUEUE_ALIGN);
    size_t total = stride * capacity;

    queue *self = (queue *)malloc(sizeof(queue));
    if (!self) return NULL;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(self);
        return NULL;
    }

    self->fun       = &queue_fun;
    self->buf       = buf;
    self->elem_size = elem_size;
    self->stride    = stride;
    self->capacity  = capacity;
    self->head      = 0;
    self->tail      = 0;
    self->count     = 0;
    self->owns      = true;
    return self;
}

/* 静态分配：调用方提供后备缓冲区，对象仅做切分与绑定（owns == false） */
void queue_init(queue *self, void *buf, size_t buf_size, size_t elem_size) {
    if (!self || !buf || buf_size == 0 || elem_size == 0) return;

    /* 把后备缓冲区起点向上对齐到 QUEUE_ALIGN，保证每个槽首字节对齐安全 */
    uintptr_t raw     = (uintptr_t)buf;
    uintptr_t aligned = (raw + QUEUE_ALIGN - 1U) & ~(uintptr_t)(QUEUE_ALIGN - 1U);
    size_t avail = (aligned <= raw + buf_size) ? (size_t)(raw + buf_size - aligned) : 0;

    size_t stride = QUEUE_ROUND_UP(elem_size, QUEUE_ALIGN);
    size_t cap    = avail / stride;

    self->fun       = &queue_fun;
    self->buf       = (uint8_t *)aligned;
    self->elem_size = elem_size;
    self->stride    = stride;
    self->capacity  = cap;
    self->head      = 0;
    self->tail      = 0;
    self->count     = 0;
    self->owns      = false;
}

void queue_deinit(queue *self) {
    if (!self) return;
    self->fun->deinit(self);
}

void queue_destroy(queue *self) {
    if (!self) return;
    self->fun->destroy(self);
}
