#include "common/ringbuffer.h"
#include <stdlib.h>
#include <string.h>

/* --- internal implementations (the real bodies, referenced by the vtable) --- */

static int  rb_put(ringbuffer *self, uint8_t c);
static int  rb_get(ringbuffer *self, uint8_t *c);
static size_t rb_write(ringbuffer *self, const void *data, size_t len);
static size_t rb_read(ringbuffer *self, void *data, size_t len);
static int  rb_peek(ringbuffer *self, size_t index, uint8_t *c);
static size_t rb_available(ringbuffer *self);
static size_t rb_free_space(ringbuffer *self);
static int  rb_is_empty(ringbuffer *self);
static int  rb_is_full(ringbuffer *self);
static void rb_clear(ringbuffer *self);
static size_t rb_capacity(ringbuffer *self);
static void rb_set_overwrite(ringbuffer *self, int enable);

/* one shared vtable for the whole ring buffer class */
static const struct ringbufferVtable ringbuffer_vtable = {
    .put         = rb_put,
    .get         = rb_get,
    .write       = rb_write,
    .read        = rb_read,
    .peek        = rb_peek,
    .available   = rb_available,
    .free_space  = rb_free_space,
    .is_empty    = rb_is_empty,
    .is_full     = rb_is_full,
    .clear       = rb_clear,
    .capacity    = rb_capacity,
    .set_overwrite = rb_set_overwrite,
};

/* public methods — thin wrappers that dispatch through the vtable (OOC) */
static int  rb_fun_put(ringbuffer *self, uint8_t c)
    { return (self && self->vtable) ? self->vtable->put(self, c) : -1; }
static int  rb_fun_get(ringbuffer *self, uint8_t *c)
    { return (self && self->vtable) ? self->vtable->get(self, c) : -1; }
static size_t rb_fun_write(ringbuffer *self, const void *data, size_t len)
    { return (self && self->vtable) ? self->vtable->write(self, data, len) : 0; }
static size_t rb_fun_read(ringbuffer *self, void *data, size_t len)
    { return (self && self->vtable) ? self->vtable->read(self, data, len) : 0; }
static int  rb_fun_peek(ringbuffer *self, size_t index, uint8_t *c)
    { return (self && self->vtable) ? self->vtable->peek(self, index, c) : -1; }
static size_t rb_fun_available(ringbuffer *self)
    { return (self && self->vtable) ? self->vtable->available(self) : 0; }
static size_t rb_fun_free_space(ringbuffer *self)
    { return (self && self->vtable) ? self->vtable->free_space(self) : 0; }
static int  rb_fun_is_empty(ringbuffer *self)
    { return (self && self->vtable) ? self->vtable->is_empty(self) : 1; }
static int  rb_fun_is_full(ringbuffer *self)
    { return (self && self->vtable) ? self->vtable->is_full(self) : 0; }
static void rb_fun_clear(ringbuffer *self)
    { if (self && self->vtable) self->vtable->clear(self); }
static size_t rb_fun_capacity(ringbuffer *self)
    { return (self && self->vtable) ? self->vtable->capacity(self) : 0; }
static void rb_fun_set_overwrite(ringbuffer *self, int enable)
    { if (self && self->vtable) self->vtable->set_overwrite(self, enable); }

const struct ringbufferFun ringbuffer_fun = {
    .destroy      = ringbuffer_destroy,
    .init         = ringbuffer_init,
    .deinit       = ringbuffer_deinit,
    .put          = rb_fun_put,
    .get          = rb_fun_get,
    .write        = rb_fun_write,
    .read         = rb_fun_read,
    .peek         = rb_fun_peek,
    .available    = rb_fun_available,
    .free_space   = rb_fun_free_space,
    .is_empty     = rb_fun_is_empty,
    .is_full      = rb_fun_is_full,
    .clear        = rb_fun_clear,
    .capacity     = rb_fun_capacity,
    .set_overwrite = rb_fun_set_overwrite,
};

ringbuffer *ringbuffer_create(const void *config)
{
    const ringbuffer_config_t *c = (const ringbuffer_config_t *)config;
    if (!c || c->size < 2) return NULL;          /* need >= 2 slots */

    ringbuffer *self = (ringbuffer *)malloc(sizeof(ringbuffer));
    if (!self) return NULL;
    memset(self, 0, sizeof(ringbuffer));

    if (c->buf) {
        self->buf = c->buf;
        self->owns_buf = 0;
    } else {
        self->buf = (uint8_t *)malloc(c->size);
        if (!self->buf) { free(self); return NULL; }
        self->owns_buf = 1;
    }
    self->size = c->size;
    self->overwrite = c->overwrite ? 1 : 0;
    ringbuffer_init(self);
    return self;
}

void ringbuffer_destroy(ringbuffer *self)
{
    if (!self) return;
    ringbuffer_deinit(self);
    if (self->owns_buf && self->buf) free(self->buf);
    free(self);
}

void ringbuffer_init(ringbuffer *self)
{
    if (!self) return;
    self->vtable = &ringbuffer_vtable;   /* shared per-class vtable */
    self->fun = &ringbuffer_fun;
    self->head = 0;
    self->tail = 0;
    /* buf / size / overwrite / owns_buf are set by create() before init() */
}

void ringbuffer_deinit(ringbuffer *self)
{
    (void)self;   /* no per-instance allocation to free (buf is external or freed in destroy) */
}

/* --- core ring math (SPSC: producer owns head, consumer owns tail) --- */

static int rb_is_full(ringbuffer *self)
{
    return ((self->head + 1U) % self->size) == self->tail;
}
static int rb_is_empty(ringbuffer *self)
{
    return self->head == self->tail;
}
static size_t rb_available(ringbuffer *self)
{
    return (self->head >= self->tail)
           ? (self->head - self->tail)
           : (self->size - self->tail + self->head);
}
static size_t rb_free_space(ringbuffer *self)
{
    /* one slot is reserved to distinguish full from empty */
    return self->size - rb_available(self) - 1U;
}
static size_t rb_capacity(ringbuffer *self)
{
    return self->size - 1U;
}
static void rb_clear(ringbuffer *self)
{
    self->head = 0;
    self->tail = 0;
}
static void rb_set_overwrite(ringbuffer *self, int enable)
{
    self->overwrite = enable ? 1 : 0;
}

static int rb_put(ringbuffer *self, uint8_t c)
{
    if (!self || !self->buf) return -1;
    size_t next = (self->head + 1U) % self->size;
    if (next == self->tail) {                 /* full */
        if (!self->overwrite) return -1;       /* no-overwrite: reject */
        self->tail = (self->tail + 1U) % self->size;  /* overwrite: drop oldest */
    }
    self->buf[self->head] = c;
    self->head = next;
    return 0;
}

static int rb_get(ringbuffer *self, uint8_t *c)
{
    if (!self || !self->buf || !c) return -1;
    if (self->head == self->tail) return -1;   /* empty */
    *c = self->buf[self->tail];
    self->tail = (self->tail + 1U) % self->size;
    return 0;
}

static size_t rb_write(ringbuffer *self, const void *data, size_t len)
{
    if (!self || !self->buf || !data) return 0;
    const uint8_t *p = (const uint8_t *)data;
    size_t stored = 0;
    for (size_t i = 0; i < len; i++) {
        if (rb_put(self, p[i]) == 0) stored++;
        else if (!self->overwrite) break;      /* no space: stop early */
        /* overwrite mode: rb_put already advanced tail, keep going */
    }
    return stored;
}

static size_t rb_read(ringbuffer *self, void *data, size_t len)
{
    if (!self || !self->buf || !data) return 0;
    uint8_t *p = (uint8_t *)data;
    size_t got = 0;
    for (size_t i = 0; i < len; i++) {
        if (rb_get(self, &p[i]) == 0) got++;
        else break;                            /* empty: stop */
    }
    return got;
}

static int rb_peek(ringbuffer *self, size_t index, uint8_t *c)
{
    if (!self || !self->buf || !c) return -1;
    if (index >= rb_available(self)) return -1;
    size_t idx = (self->tail + index) % self->size;
    *c = self->buf[idx];
    return 0;
}

/* --- on-board self-test for the class itself --- */
int ringbuffer_run_selftest(void)
{
    uint8_t storage[8];
    ringbuffer_config_t cfg = { .buf = storage, .size = sizeof(storage), .overwrite = 0 };
    ringbuffer *rb = ringbuffer_create(&cfg);
    if (!rb) return 0;

    int ok = 1;
    uint8_t c;

    /* empty state */
    if (!rb->fun->is_empty(rb)) ok = 0;
    if (rb->fun->get(rb, &c) != -1) ok = 0;          /* get on empty must fail */
    if (rb->fun->capacity(rb) != 7) ok = 0;          /* usable = size - 1 */

    /* fill to capacity (7) */
    for (uint8_t i = 0; i < 7; i++)
        if (rb->fun->put(rb, i) != 0) ok = 0;
    if (rb->fun->is_full(rb) != 1) ok = 0;
    if (rb->fun->available(rb) != 7) ok = 0;
    if (rb->fun->put(rb, 0xAA) != -1) ok = 0;        /* full + no-overwrite rejects */

    /* drain in FIFO order */
    for (uint8_t i = 0; i < 7; i++) {
        if (rb->fun->get(rb, &c) != 0 || c != i) ok = 0;
    }
    if (!rb->fun->is_empty(rb)) ok = 0;

    /* block write + peek + block read */
    uint8_t w[3] = { 10, 20, 30 };
    if (rb->fun->write(rb, w, 3) != 3) ok = 0;
    if (rb->fun->peek(rb, 1, &c) != 0 || c != 20) ok = 0;
    uint8_t r[3];
    if (rb->fun->read(rb, r, 3) != 3 || r[0] != 10 || r[1] != 20 || r[2] != 30) ok = 0;

    /* overwrite mode: filling past capacity drops the oldest */
    rb->fun->set_overwrite(rb, 1);
    rb->fun->clear(rb);
    for (uint8_t i = 0; i < 7; i++) rb->fun->put(rb, i);
    if (rb->fun->put(rb, 0x99) != 0) ok = 0;         /* overwrites byte 0 */
    if (rb->fun->get(rb, &c) != 0 || c != 1) ok = 0; /* oldest (0) was dropped */

    ringbuffer_destroy(rb);
    return ok;
}
