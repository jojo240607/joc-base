#include "pool.h"

#include <stdlib.h>
#include <string.h>

/* 空闲块对齐粒度：至少要能容下一个 void* 指针，以便把 next 指针存在块首 */
#define POOL_ALIGN (sizeof(void *))

/* 向上取整到 align 的整数倍 */
#define POOL_ROUND_UP(n, a) (((n) + (a) - 1) / (a) * (a))

/* ---------------------------------------------------------------------------
 * 私有辅助：用空闲块把后备缓冲区串成单链表
 * （每个空闲块首字节写入“下一块”指针，末块写入 NULL）
 * ------------------------------------------------------------------------- */
static void pool_build(pool *self) {
    self->free_list = NULL;
    self->used = 0;
    if (self->capacity == 0) return;

    uint8_t *p = self->base;
    for (size_t i = 0; i < self->capacity; i++) {
        void *next = (i + 1U < self->capacity) ? (void *)(p + self->stride) : NULL;
        *(void **)p = next;
        p += self->stride;
    }
    self->free_list = self->base;   /* 链表头指向第一块 */
}

/* ---------------------------------------------------------------------------
 * vtable 方法实现：全部行为集中在虚表里，pool 成为真正的对象
 * ------------------------------------------------------------------------- */

static void *pool_method_alloc(pool *self) {
    if (self->free_list == NULL) return NULL;   /* 池满 */
    void *p = self->free_list;
    self->free_list = *(void **)p;              /* 取下一块 */
    self->used++;
    return p;
}

static bool pool_method_free(pool *self, void *ptr) {
    if (!ptr) return false;
    if (!self->fun->in_pool(self, ptr)) return false;   /* 不属于本池 */
    *(void **)ptr = self->free_list;                    /* 头插回空闲链表 */
    self->free_list = ptr;
    self->used--;
    return true;
}

static bool pool_method_in_pool(const pool *self, const void *ptr) {
    if (!ptr) return false;
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t b = (uintptr_t)self->base;
    if (p < b) return false;
    uintptr_t off = p - b;
    if (off >= self->capacity * self->stride) return false;  /* 越界 */
    if (off % self->stride != 0) return false;              /* 未按块对齐 */
    return true;
}

static size_t pool_method_capacity(const pool *self) {
    return self->capacity;
}

static size_t pool_method_used(const pool *self) {
    return self->used;
}

static size_t pool_method_available(const pool *self) {
    return self->capacity - self->used;
}

static bool pool_method_empty(const pool *self) {
    return self->used == 0;
}

static bool pool_method_full(const pool *self) {
    return self->used == self->capacity;
}

static void pool_method_reset(pool *self) {
    pool_build(self);   /* 重建空闲链表，等价回收全部块 */
}

static void pool_method_deinit(pool *self) {
    /* 缓冲区由对象 heap 分配时才释放；用户静态缓冲区（owns==false）不动 */
    if (self->owns && self->base) {
        free(self->base);
        self->base = NULL;
    }
    self->capacity = 0;
}

static void pool_method_destroy(pool *self) {
    if (!self) return;
    self->fun->deinit(self);   /* 先释放后备缓冲区（若 owns） */
    free(self);
}

/* ---------------------------------------------------------------------------
 * 方法表实例
 * ------------------------------------------------------------------------- */
const struct poolFun pool_fun = {
    .alloc     = pool_method_alloc,
    .free      = pool_method_free,
    .in_pool   = pool_method_in_pool,
    .capacity  = pool_method_capacity,
    .used      = pool_method_used,
    .available = pool_method_available,
    .empty     = pool_method_empty,
    .full      = pool_method_full,
    .reset     = pool_method_reset,
    .deinit    = pool_method_deinit,
    .destroy   = pool_method_destroy,
};

/* ---------------------------------------------------------------------------
 * 构造函数 / 工厂（自由函数，符合 OOC 惯例）
 * ------------------------------------------------------------------------- */

/* 堆分配：对象本身 + 后备缓冲区均由 malloc 获得（owns == true） */
pool *pool_create(size_t block_size, size_t block_count) {
    if (block_size == 0 || block_count == 0) return NULL;

    size_t stride = POOL_ROUND_UP(block_size, POOL_ALIGN);
    size_t total = stride * block_count;

    pool *self = (pool *)malloc(sizeof(pool));
    if (!self) return NULL;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(self);
        return NULL;
    }

    self->fun       = &pool_fun;
    self->base      = buf;
    self->block_size = block_size;
    self->stride     = stride;
    self->capacity   = block_count;
    self->used       = 0;
    self->owns       = true;
    pool_build(self);
    return self;
}

/* 静态分配：调用方提供后备缓冲区，对象仅做切分与绑定（owns == false） */
void pool_init(pool *self, void *buf, size_t buf_size, size_t block_size) {
    if (!self || !buf || buf_size == 0 || block_size == 0) return;

    /* 把后备缓冲区起点向上对齐到 POOL_ALIGN，保证每块首字节可安全写入指针 */
    uintptr_t raw    = (uintptr_t)buf;
    uintptr_t aligned = (raw + POOL_ALIGN - 1U) & ~(uintptr_t)(POOL_ALIGN - 1U);
    size_t avail = (aligned <= raw + buf_size) ? (size_t)(raw + buf_size - aligned) : 0;

    size_t stride = POOL_ROUND_UP(block_size, POOL_ALIGN);
    size_t cap    = avail / stride;

    self->fun        = &pool_fun;
    self->base       = (uint8_t *)aligned;
    self->block_size = block_size;
    self->stride     = stride;
    self->capacity   = cap;
    self->used       = 0;
    self->owns       = false;
    pool_build(self);
}

void pool_deinit(pool *self) {
    if (!self) return;
    self->fun->deinit(self);
}

void pool_destroy(pool *self) {
    if (!self) return;
    self->fun->destroy(self);
}
