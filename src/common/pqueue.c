#include "pqueue.h"

#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * 私有：二叉最小堆的上浮 / 下沉
 * cmp(a, b) < 0 表示 a 优先级更高（堆序上更小），故堆顶为最小元素。
 * ------------------------------------------------------------------------- */
static void pqueue_sift_up(pqueue *self, size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1U) / 2U;
        if (self->cmp(self->buf[idx], self->buf[parent]) < 0) {
            void *tmp        = self->buf[idx];
            self->buf[idx]   = self->buf[parent];
            self->buf[parent] = tmp;
            idx = parent;
        } else {
            break;
        }
    }
}

static void pqueue_sift_down(pqueue *self, size_t idx) {
    for (;;) {
        size_t left    = 2U * idx + 1U;
        size_t right   = 2U * idx + 2U;
        size_t smallest = idx;

        if (left < self->count &&
            self->cmp(self->buf[left], self->buf[smallest]) < 0) {
            smallest = left;
        }
        if (right < self->count &&
            self->cmp(self->buf[right], self->buf[smallest]) < 0) {
            smallest = right;
        }
        if (smallest == idx) break;

        void *tmp          = self->buf[idx];
        self->buf[idx]     = self->buf[smallest];
        self->buf[smallest] = tmp;
        idx = smallest;
    }
}

/* ---------------------------------------------------------------------------
 * vtable 方法实现：全部行为集中在虚表里，pqueue 成为真正的对象
 * ------------------------------------------------------------------------- */

static bool pqueue_method_push(pqueue *self, void *item) {
    if (!self || self->cmp == NULL) return false;
    if (self->count == self->capacity) return false;   /* 满 */

    self->buf[self->count] = item;
    pqueue_sift_up(self, self->count);
    self->count++;
    return true;
}

static bool pqueue_method_pop(pqueue *self, void **item) {
    if (!self || self->count == 0) return false;       /* 空 */

    void *root = self->buf[0];
    self->count--;
    if (self->count > 0) {
        self->buf[0] = self->buf[self->count];         /* 末元素移到堆顶 */
        pqueue_sift_down(self, 0);
    }
    if (item) *item = root;
    return true;
}

static bool pqueue_method_peek(const pqueue *self, void **item) {
    if (!self || self->count == 0) return false;       /* 空 */
    if (item) *item = self->buf[0];
    return true;
}

static size_t pqueue_method_capacity(const pqueue *self) {
    return self ? self->capacity : 0;
}

static size_t pqueue_method_size(const pqueue *self) {
    return self ? self->count : 0;
}

static bool pqueue_method_empty(const pqueue *self) {
    return self ? self->count == 0 : true;
}

static bool pqueue_method_full(const pqueue *self) {
    return self ? self->count == self->capacity : true;
}

static void pqueue_method_clear(pqueue *self) {
    if (!self) return;
    self->count = 0;
}

static void pqueue_method_deinit(pqueue *self) {
    if (!self) return;
    /* 缓冲区由对象 heap 分配时才释放；用户静态缓冲区（owns==false）不动 */
    if (self->owns && self->buf) {
        free(self->buf);
        self->buf = NULL;
    }
    self->capacity = 0;
    self->cmp = NULL;
}

static void pqueue_method_destroy(pqueue *self) {
    if (!self) return;
    self->fun->deinit(self);   /* 先释放后备缓冲区（若 owns） */
    free(self);
}

/* ---------------------------------------------------------------------------
 * 方法表实例
 * ------------------------------------------------------------------------- */
const struct pqueueFun pqueue_fun = {
    .push     = pqueue_method_push,
    .pop      = pqueue_method_pop,
    .peek     = pqueue_method_peek,
    .capacity = pqueue_method_capacity,
    .size     = pqueue_method_size,
    .empty    = pqueue_method_empty,
    .full     = pqueue_method_full,
    .clear    = pqueue_method_clear,
    .deinit   = pqueue_method_deinit,
    .destroy  = pqueue_method_destroy,
};

/* ---------------------------------------------------------------------------
 * 构造函数 / 工厂（自由函数，符合 OOC 惯例）
 * ------------------------------------------------------------------------- */

/* 堆分配：对象本身 + 后备缓冲区均由 malloc 获得（owns == true） */
pqueue *pqueue_create(size_t capacity, pqueue_cmp cmp) {
    if (capacity == 0 || cmp == NULL) return NULL;

    pqueue *self = (pqueue *)malloc(sizeof(pqueue));
    if (!self) return NULL;

    void **buf = (void **)malloc(capacity * sizeof(void *));
    if (!buf) {
        free(self);
        return NULL;
    }

    self->fun      = &pqueue_fun;
    self->buf      = buf;
    self->capacity = capacity;
    self->count    = 0;
    self->cmp      = cmp;
    self->owns     = true;
    return self;
}

/* 静态分配：调用方提供一段 void* 数组作为后备存储（owns == false） */
void pqueue_init(pqueue *self, void *buf, size_t buf_size, pqueue_cmp cmp) {
    if (!self || !buf || buf_size == 0 || cmp == NULL) return;

    /* 容量 = 后备字节数可容纳的 void* 槽位数 */
    size_t cap = buf_size / sizeof(void *);

    self->fun      = &pqueue_fun;
    self->buf      = (void **)buf;
    self->capacity = cap;
    self->count    = 0;
    self->cmp      = cmp;
    self->owns     = false;
}

void pqueue_deinit(pqueue *self) {
    if (!self) return;
    self->fun->deinit(self);
}

void pqueue_destroy(pqueue *self) {
    if (!self) return;
    self->fun->destroy(self);
}
