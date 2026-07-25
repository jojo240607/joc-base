#include "ring.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * vtable 方法实现：全部行为集中在虚表里，ring 成为真正的对象
 * ------------------------------------------------------------------------- */

static bool ring_method_empty(const ring *self) {
    return self->count == 0;
}

static bool ring_method_full(const ring *self) {
    return self->count == self->capacity;
}

static size_t ring_method_capacity(const ring *self) {
    return self->capacity;
}

static size_t ring_method_size(const ring *self) {
    return self->count;
}

static size_t ring_method_space(const ring *self) {
    return self->capacity - self->count;
}

static bool ring_method_push(ring *self, uint8_t byte) {
    if (self->count == self->capacity) return false;   /* 满 */
    self->buf[self->head] = byte;
    self->head = (self->head + 1) % self->capacity;
    self->count++;
    return true;
}

static bool ring_method_pop(ring *self, uint8_t *byte) {
    if (self->count == 0) return false;                /* 空 */
    if (byte) *byte = self->buf[self->tail];
    self->tail = (self->tail + 1) % self->capacity;
    self->count--;
    return true;
}

static bool ring_method_peek(const ring *self, uint8_t *byte) {
    if (self->count == 0) return false;                /* 空 */
    if (byte) *byte = self->buf[self->tail];
    return true;
}

/* 批量写入：受剩余空间限制，跨尾边界自动分段拷贝 */
static size_t ring_method_write(ring *self, const void *src, size_t len) {
    const uint8_t *p = (const uint8_t *)src;
    size_t to_write = len;
    if (to_write > self->capacity - self->count) {
        to_write = self->capacity - self->count;
    }

    size_t written = 0;
    while (written < to_write) {
        size_t first = self->capacity - self->head;    /* head 到缓冲区末尾的连续空间 */
        if (first > to_write - written) first = to_write - written;
        memcpy(self->buf + self->head, p + written, first);
        self->head = (self->head + first) % self->capacity;
        written += first;
    }
    self->count += written;
    return written;
}

/* 批量读取：受已存数据量限制，跨尾边界自动分段拷贝 */
static size_t ring_method_read(ring *self, void *dst, size_t len) {
    uint8_t *p = (uint8_t *)dst;
    size_t to_read = len;
    if (to_read > self->count) to_read = self->count;

    size_t read = 0;
    while (read < to_read) {
        size_t first = self->capacity - self->tail;    /* tail 到缓冲区末尾的连续数据 */
        if (first > to_read - read) first = to_read - read;
        memcpy(p + read, self->buf + self->tail, first);
        self->tail = (self->tail + first) % self->capacity;
        read += first;
    }
    self->count -= read;
    return read;
}

static void ring_method_clear(ring *self) {
    self->head  = 0;
    self->tail  = 0;
    self->count = 0;
}

static void ring_method_deinit(ring *self) {
    /* 缓冲区由对象 heap 分配时才释放；用户静态缓冲区（owns==false）不动 */
    if (self->owns && self->buf) {
        free(self->buf);
        self->buf = NULL;
    }
    self->capacity = 0;
}

static void ring_method_destroy(ring *self) {
    if (!self) return;
    self->fun->deinit(self);   /* 先释放后备缓冲区（若 owns） */
    free(self);
}

/* ---------------------------------------------------------------------------
 * 方法表实例
 * ------------------------------------------------------------------------- */
const struct ringFun ring_fun = {
    .empty     = ring_method_empty,
    .full      = ring_method_full,
    .capacity  = ring_method_capacity,
    .size      = ring_method_size,
    .space     = ring_method_space,
    .push      = ring_method_push,
    .pop       = ring_method_pop,
    .peek      = ring_method_peek,
    .write     = ring_method_write,
    .read      = ring_method_read,
    .clear     = ring_method_clear,
    .deinit    = ring_method_deinit,
    .destroy   = ring_method_destroy,
};

/* ---------------------------------------------------------------------------
 * 构造函数 / 工厂（自由函数，符合 OOC 惯例）
 * ------------------------------------------------------------------------- */

/* 堆分配：对象本身 + 后备缓冲区均由 malloc 获得（owns == true） */
ring *ring_create(size_t capacity) {
    if (capacity == 0) return NULL;

    ring *self = (ring *)malloc(sizeof(ring));
    if (!self) return NULL;

    uint8_t *buf = (uint8_t *)malloc(capacity);
    if (!buf) {
        free(self);
        return NULL;
    }

    self->fun      = &ring_fun;
    self->buf      = buf;
    self->capacity = capacity;
    self->head     = 0;
    self->tail     = 0;
    self->count    = 0;
    self->owns     = true;
    return self;
}

/* 静态分配：调用方提供后备缓冲区（数组），对象仅做绑定（owns == false） */
void ring_init(ring *self, uint8_t *buf, size_t capacity) {
    if (!self || !buf || capacity == 0) return;

    self->fun      = &ring_fun;
    self->buf      = buf;
    self->capacity = capacity;
    self->head     = 0;
    self->tail     = 0;
    self->count    = 0;
    self->owns     = false;
}

void ring_deinit(ring *self) {
    if (!self) return;
    self->fun->deinit(self);
}

void ring_destroy(ring *self) {
    if (!self) return;
    self->fun->destroy(self);
}
