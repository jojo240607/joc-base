#include "bitmap.h"

#include <stdlib.h>
#include <string.h>

/* 每字节置位个数查表（popcount），避免逐位循环，提升大位图统计效率 */
static const uint8_t BITMAP_POPCOUNT[256] = {
    0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4, 1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5, 2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5, 2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6, 3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5, 2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6, 3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6, 3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7, 4,5,5,6,5,6,6,7,5,6,6,7,6,7,7,8,
};

/* 字节内最低置位的位置（无置位返回 8） */
static inline unsigned bitmap_ctz_byte(uint8_t b) {
    unsigned i = 0;
    while (i < 8U && (b & (1U << i)) == 0U) i++;
    return i;
}

/* 字节内最低清零的位置（无清零返回 8） */
static inline unsigned bitmap_ctz_zero_byte(uint8_t b) {
    unsigned i = 0;
    while (i < 8U && ((b & (1U << i)) != 0U)) i++;
    return i;
}

/* 用于“有效位非 8 整数倍”的末字节掩码（仅保留有效低位） */
static inline uint8_t bitmap_last_mask(size_t num_bits) {
    size_t rem = num_bits % 8U;
    return (rem == 0U) ? (uint8_t)0xFF : (uint8_t)((1U << rem) - 1U);
}

/* ---------------------------------------------------------------------------
 * vtable 方法实现
 * ------------------------------------------------------------------------- */
static bool bitmap_method_set(bitmap *self, size_t index) {
    if (index >= self->num_bits) return false;   /* 越界 */
    self->bits[index / 8U] |= (uint8_t)(1U << (index % 8U));
    return true;
}

static bool bitmap_method_reset(bitmap *self, size_t index) {
    if (index >= self->num_bits) return false;
    self->bits[index / 8U] &= (uint8_t)~(1U << (index % 8U));
    return true;
}

static bool bitmap_method_toggle(bitmap *self, size_t index) {
    if (index >= self->num_bits) return false;
    self->bits[index / 8U] ^= (uint8_t)(1U << (index % 8U));
    return true;
}

static bool bitmap_method_test(const bitmap *self, size_t index) {
    if (index >= self->num_bits) return false;
    return (self->bits[index / 8U] >> (index % 8U)) & 1U;
}

static void bitmap_method_set_all(bitmap *self) {
    size_t full = self->num_bits / 8U;
    for (size_t i = 0; i < full; i++) self->bits[i] = 0xFF;
    size_t rem = self->num_bits % 8U;
    if (rem) self->bits[full] = (uint8_t)((1U << rem) - 1U);  /* 仅置有效低位 */
}

static void bitmap_method_clear(bitmap *self) {
    memset(self->bits, 0, (self->num_bits + 7U) / 8U);
}

static size_t bitmap_method_capacity(const bitmap *self) {
    return self->num_bits;
}

static size_t bitmap_method_count_set(const bitmap *self) {
    size_t full = self->num_bits / 8U;
    size_t sum = 0;
    for (size_t i = 0; i < full; i++) sum += BITMAP_POPCOUNT[self->bits[i]];
    size_t rem = self->num_bits % 8U;
    if (rem) {
        uint8_t last = (uint8_t)(self->bits[full] & ((1U << rem) - 1U));
        sum += BITMAP_POPCOUNT[last];
    }
    return sum;
}

static bool bitmap_method_any_set(const bitmap *self) {
    return self->fun->count_set(self) != 0U;
}

static bool bitmap_method_none_set(const bitmap *self) {
    return self->fun->count_set(self) == 0U;
}

static bool bitmap_method_all_set(const bitmap *self) {
    size_t full = self->num_bits / 8U;
    for (size_t i = 0; i < full; i++) if (self->bits[i] != 0xFF) return false;
    size_t rem = self->num_bits % 8U;
    if (rem) {
        uint8_t want = (uint8_t)((1U << rem) - 1U);
        if ((self->bits[full] & want) != want) return false;
    }
    return true;
}

static bool bitmap_method_find_first_set(const bitmap *self, size_t *index) {
    size_t full = self->num_bits / 8U;
    for (size_t i = 0; i < full; i++) {
        uint8_t b = self->bits[i];
        if (b) { *index = i * 8U + bitmap_ctz_byte(b); return true; }
    }
    size_t rem = self->num_bits % 8U;
    if (rem) {
        uint8_t b = (uint8_t)(self->bits[full] & ((1U << rem) - 1U));
        if (b) { *index = full * 8U + bitmap_ctz_byte(b); return true; }
    }
    return false;
}

static bool bitmap_method_find_first_clear(const bitmap *self, size_t *index) {
    size_t full = self->num_bits / 8U;
    for (size_t i = 0; i < full; i++) {
        uint8_t b = self->bits[i];
        if (b != 0xFF) { *index = i * 8U + bitmap_ctz_zero_byte(b); return true; }
    }
    size_t rem = self->num_bits % 8U;
    if (rem) {
        uint8_t want = (uint8_t)((1U << rem) - 1U);
        uint8_t b = (uint8_t)(self->bits[full] & want);
        if (b != want) { *index = full * 8U + bitmap_ctz_zero_byte((uint8_t)(b | (uint8_t)~want)); return true; }
    }
    return false;
}

static void bitmap_method_deinit(bitmap *self) {
    if (self->owns && self->bits) {
        free(self->bits);
        self->bits = NULL;
    }
    self->num_bits = 0;
}

static void bitmap_method_destroy(bitmap *self) {
    if (!self) return;
    self->fun->deinit(self);
    free(self);
}

/* ---------------------------------------------------------------------------
 * 方法表实例
 * ------------------------------------------------------------------------- */
const struct bitmapFun bitmap_fun = {
    .set             = bitmap_method_set,
    .reset           = bitmap_method_reset,
    .toggle          = bitmap_method_toggle,
    .test            = bitmap_method_test,
    .set_all         = bitmap_method_set_all,
    .clear           = bitmap_method_clear,
    .capacity        = bitmap_method_capacity,
    .count_set       = bitmap_method_count_set,
    .any_set         = bitmap_method_any_set,
    .none_set        = bitmap_method_none_set,
    .all_set         = bitmap_method_all_set,
    .find_first_set  = bitmap_method_find_first_set,
    .find_first_clear= bitmap_method_find_first_clear,
    .deinit          = bitmap_method_deinit,
    .destroy         = bitmap_method_destroy,
};

/* ---------------------------------------------------------------------------
 * 构造函数 / 工厂（自由函数）
 * ------------------------------------------------------------------------- */
bitmap *bitmap_create(size_t num_bits) {
    if (num_bits == 0) return NULL;
    size_t bytes = (num_bits + 7U) / 8U;

    bitmap *self = (bitmap *)malloc(sizeof(bitmap));
    if (!self) return NULL;

    uint8_t *buf = (uint8_t *)malloc(bytes);
    if (!buf) { free(self); return NULL; }

    self->fun      = &bitmap_fun;
    self->bits     = buf;
    self->num_bits = num_bits;
    self->owns     = true;
    memset(self->bits, 0, bytes);
    return self;
}

static void bitmap_bind(bitmap *self, void *buf, size_t buf_size, size_t num_bits) {
    (void)buf_size;   /* 仅用于校验调用方缓冲区足够大，绑定本身只用 num_bits */
    self->fun      = &bitmap_fun;
    self->bits     = (uint8_t *)buf;
    self->num_bits = num_bits;
    self->owns     = false;
    memset(self->bits, 0, (num_bits + 7U) / 8U);
}

void bitmap_init(bitmap *self, void *buf, size_t buf_size) {
    if (!self || !buf || buf_size == 0) return;
    bitmap_bind(self, buf, buf_size, buf_size * 8U);
}

void bitmap_init_n(bitmap *self, void *buf, size_t buf_size, size_t num_bits) {
    if (!self || !buf || buf_size == 0 || num_bits == 0) return;
    /* 缓冲区必须容纳得下 num_bits */
    if ((num_bits + 7U) / 8U > buf_size) { self->num_bits = 0; self->bits = NULL; return; }
    bitmap_bind(self, buf, buf_size, num_bits);
}

void bitmap_deinit(bitmap *self) {
    if (!self) return;
    self->fun->deinit(self);
}

void bitmap_destroy(bitmap *self) {
    if (!self) return;
    self->fun->destroy(self);
}
