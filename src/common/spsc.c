#include "spsc.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * 编译器屏障：阻止编译器把“数据读写”重排到“索引发布/读取”的另一侧。
 * 面向单核 MCU（ISR↔main）足矣；多核/弱内存序平台需替换为 DMB 等硬件屏障。
 * ------------------------------------------------------------------------- */
#if defined(_MSC_VER)
#include <intrin.h>
#pragma warning(disable : 4996) /* _ReadWriteBarrier 已弃用，但仍是 MSVC 下的纯编译器屏障 */
#define SPSC_CBARRIER() _ReadWriteBarrier()
#elif defined(__GNUC__) || defined(__clang__)
#define SPSC_CBARRIER() __asm__ __volatile__("" ::: "memory")
#else
#define SPSC_CBARRIER() ((void)0)
#endif

/* ---------------------------------------------------------------------------
 * 小端序 16 位读写（长度头）——逐字节访问，避免对齐/端序假设。
 * ------------------------------------------------------------------------- */
static inline uint16_t spsc_rd16(const uint8_t *b, size_t p) {
    return (uint16_t)((uint16_t)b[p] | ((uint16_t)b[p + 1] << 8));
}
static inline void spsc_wr16(uint8_t *b, size_t p, uint16_t v) {
    b[p]     = (uint8_t)(v & 0xFFu);
    b[p + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* ---------------------------------------------------------------------------
 * 放置规划：给定字节前沿 p 与消费者游标 rd，计算一条长度为 len 的消息应放置的
 * “起始偏移 start”与“占用字节数 foot”（含长度头，回绕时含尾部填充）。返回 false
 * 表示空间不足。规则保证：段尾剩余为 0 或 ≥ HEADER，杜绝放不下长度头的碎片。
 * ------------------------------------------------------------------------- */
static bool spsc_plan(size_t p, size_t rd, size_t cap, uint16_t len,
                      size_t *start, size_t *foot) {
    size_t rec, used, freeb, p_mod, end_room;
    if (cap == 0) return false;   /* 不可用缓冲（cap==0）直接判空间不足，避免除零 */

    rec    = (size_t)SPSC_HEADER_SIZE + (size_t)len;
    used   = p - rd;
    freeb  = cap - used;
    p_mod  = p % cap;
    end_room = cap - p_mod;

    /* 情形 A：尾部就地放置（连续 rec 字节，且段尾剩余为 0 或 ≥ HEADER） */
    if (end_room >= rec && (end_room == rec || end_room - rec >= SPSC_HEADER_SIZE) &&
        freeb >= rec) {
        *start = p_mod;
        *foot  = rec;
        return true;
    }

    /* 情形 B：回绕到起始（尾部写填充，消息置于 0；foot = 尾部填充 + rec） */
    {
        size_t f = end_room + rec;
        if (cap >= rec && (cap == rec || cap - rec >= SPSC_HEADER_SIZE) &&
            end_room >= SPSC_HEADER_SIZE && freeb >= f) {
            *start = 0;
            *foot  = f;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * 消费者辅助：跨过尾部填充哨兵，使 rd 指向真实记录头（或到达空）。
 * ------------------------------------------------------------------------- */
static void spsc_skip_padding(spsc *self) {
    while (self->wr != self->rd) {
        size_t pos;
        SPSC_CBARRIER();
        pos = self->rd % self->cap;
        if (spsc_rd16(self->buf, pos) != SPSC_LEN_SENTINEL) break;
        self->rd += (self->cap - pos);
    }
}

/* ===========================================================================
 * 方法实现（单生产者单消费者，无锁）
 * ========================================================================= */
static bool spsc_method_reserve(spsc *self, uint16_t len, void **pptr);
static bool spsc_method_commit(spsc *self);
static void spsc_method_abort(spsc *self);
static bool spsc_method_push(spsc *self, const void *data, uint16_t len);
static bool spsc_method_peek(spsc *self, const void **pptr, uint16_t *plen);
static bool spsc_method_consume(spsc *self);
static bool spsc_method_next_len(spsc *self, uint16_t *plen);
static bool spsc_method_pop(spsc *self, void *dst, size_t dst_cap, uint16_t *plen);
static bool spsc_method_is_empty(const spsc *self);
static size_t spsc_method_used(const spsc *self);
static size_t spsc_method_space(const spsc *self);
static size_t spsc_method_capacity(const spsc *self);
static void spsc_method_clear(spsc *self);
static void spsc_method_deinit(spsc *self);
static void spsc_method_destroy(spsc *self);

/* 生产者：预留一条消息空间（零拷贝），返回可写入消息体的指针。 */
static bool spsc_method_reserve(spsc *self, uint16_t len, void **pptr) {
    size_t rec, wr, rd, used, freeb, p, end_room, foot;

    if (!self || !self->fun || !pptr) return false;
    if (self->resv) return false;                 /* 上一次 reserve 未提交 */
    if (self->cap == 0) return false;             /* 未绑定有效缓冲区 */
    if (len == SPSC_LEN_SENTINEL) return false;   /* 0xFFFF 为保留标记，不可作为消息长度 */

    rec  = (size_t)SPSC_HEADER_SIZE + (size_t)len;
    wr   = self->wr;
    rd   = self->rd;
    used = wr - rd;
    freeb = self->cap - used;
    p        = wr % self->cap;
    end_room = self->cap - p;

    if (end_room >= rec && (end_room == rec || end_room - rec >= SPSC_HEADER_SIZE) &&
        freeb >= rec) {
        spsc_wr16(self->buf, p, len);
        *pptr = self->buf + p + SPSC_HEADER_SIZE;
        self->resv_foot = rec;
        self->resv = true;
        return true;
    }

    foot = end_room + rec;
    if (self->cap >= rec && (self->cap == rec || self->cap - rec >= SPSC_HEADER_SIZE) &&
        end_room >= SPSC_HEADER_SIZE && freeb >= foot) {
        spsc_wr16(self->buf, p, SPSC_LEN_SENTINEL);
        spsc_wr16(self->buf, 0, len);
        *pptr = self->buf + SPSC_HEADER_SIZE;
        self->resv_foot = foot;
        self->resv = true;
        return true;
    }
    return false;
}

static bool spsc_method_commit(spsc *self) {
    if (!self || !self->fun) return false;
    if (!self->resv) return false;
    SPSC_CBARRIER();
    self->wr += self->resv_foot;
    self->resv = false;
    self->resv_foot = 0;
    return true;
}

static void spsc_method_abort(spsc *self) {
    if (!self || !self->fun) return;
    self->resv = false;
    self->resv_foot = 0;
}

static bool spsc_method_push(spsc *self, const void *data, uint16_t len) {
    void *dst = NULL;
    if (!self || !self->fun) return false;
    if (len > 0 && !data) return false;
    if (!spsc_method_reserve(self, len, &dst)) return false;
    if (len > 0) memcpy(dst, data, len);
    return spsc_method_commit(self);
}

static bool spsc_method_peek(spsc *self, const void **pptr, uint16_t *plen) {
    size_t pos;
    uint16_t len;
    if (!self || !self->fun) return false;
    if (self->cap == 0) return false;

    spsc_skip_padding(self);
    if (self->wr == self->rd) return false;

    SPSC_CBARRIER();
    pos = self->rd % self->cap;
    len = spsc_rd16(self->buf, pos);
    if (pptr) *pptr = self->buf + pos + SPSC_HEADER_SIZE;
    if (plen) *plen = len;
    return true;
}

static bool spsc_method_consume(spsc *self) {
    size_t pos;
    uint16_t len;
    if (!self || !self->fun) return false;
    if (self->cap == 0) return false;

    spsc_skip_padding(self);
    if (self->wr == self->rd) return false;

    pos = self->rd % self->cap;
    len = spsc_rd16(self->buf, pos);
    self->rd += (size_t)SPSC_HEADER_SIZE + (size_t)len;
    spsc_skip_padding(self);
    return true;
}

static bool spsc_method_next_len(spsc *self, uint16_t *plen) {
    const void *src = NULL;
    uint16_t len = 0;
    if (!spsc_method_peek(self, &src, &len)) return false;
    if (plen) *plen = len;
    return true;
}

static bool spsc_method_pop(spsc *self, void *dst, size_t dst_cap, uint16_t *plen) {
    const void *src = NULL;
    uint16_t len = 0;
    if (!spsc_method_peek(self, &src, &len)) return false;
    if (plen) *plen = len;
    if ((size_t)len > dst_cap) return false;
    if (len > 0 && dst) memcpy(dst, src, len);
    return spsc_method_consume(self);
}

static bool spsc_method_is_empty(const spsc *self) {
    if (!self || !self->fun) return true;
    return self->wr == self->rd;
}

static size_t spsc_method_used(const spsc *self) {
    if (!self || !self->fun) return 0;
    return (size_t)(self->wr - self->rd);
}

static size_t spsc_method_space(const spsc *self) {
    if (!self || !self->fun) return 0;
    return self->cap - (size_t)(self->wr - self->rd);
}

static size_t spsc_method_capacity(const spsc *self) {
    if (!self || !self->fun) return 0;
    return self->cap;
}

static void spsc_method_clear(spsc *self) {
    if (!self || !self->fun) return;
    self->wr = 0;
    self->rd = 0;
    self->resv = false;
    self->resv_foot = 0;
}

static void spsc_method_deinit(spsc *self) {
    if (!self) return;
    if (self->owns && self->buf) {
        free(self->buf);
        self->buf = NULL;
    }
    self->cap = 0;
    self->wr = 0;
    self->rd = 0;
    self->resv = false;
    self->resv_foot = 0;
}

static void spsc_method_destroy(spsc *self) {
    if (!self) return;
    self->fun->deinit(self);
    free(self);
}

/* ===========================================================================
 * 方法表实例
 * ========================================================================= */
const struct spscFun spsc_fun = {
    .reserve  = spsc_method_reserve,
    .commit   = spsc_method_commit,
    .abort    = spsc_method_abort,
    .push     = spsc_method_push,
    .peek     = spsc_method_peek,
    .consume  = spsc_method_consume,
    .next_len = spsc_method_next_len,
    .pop      = spsc_method_pop,
    .is_empty = spsc_method_is_empty,
    .used     = spsc_method_used,
    .space    = spsc_method_space,
    .capacity = spsc_method_capacity,
    .clear    = spsc_method_clear,
    .deinit   = spsc_method_deinit,
    .destroy  = spsc_method_destroy,
};

/* ===========================================================================
 * 构造函数 / 工厂（自由函数）
 * ========================================================================= */
spsc *spsc_create(size_t capacity) {
    spsc *self;
    uint8_t *buf;
    if (capacity < SPSC_HEADER_SIZE) return NULL;

    self = (spsc *)malloc(sizeof(spsc));
    if (!self) return NULL;

    buf = (uint8_t *)malloc(capacity);
    if (!buf) { free(self); return NULL; }

    spsc_init(self, buf, capacity);
    self->owns = true;   /* 由本对象自管理，deinit 时释放 */
    return self;
}

void spsc_init(spsc *self, void *buf, size_t buf_size) {
    if (!self) return;
    if (!buf || buf_size < SPSC_HEADER_SIZE) {
        self->fun       = &spsc_fun;
        self->buf       = NULL;
        self->cap       = 0;
        self->wr        = 0;
        self->rd        = 0;
        self->resv_foot = 0;
        self->resv      = false;
        self->owns      = false;
        return;
    }
    self->fun       = &spsc_fun;
    self->buf       = (uint8_t *)buf;
    self->cap       = buf_size;
    self->wr        = 0;
    self->rd        = 0;
    self->resv_foot = 0;
    self->resv      = false;
    self->owns      = false;
}

void spsc_deinit(spsc *self) {
    if (!self) return;
    self->fun->deinit(self);
}

void spsc_destroy(spsc *self) {
    if (!self) return;
    self->fun->destroy(self);
}
