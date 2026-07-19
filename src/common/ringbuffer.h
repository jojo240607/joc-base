#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * 循环缓冲 (Ring buffer) — a reusable, platform-independent OOC utility class.
 *
 * Placed in src/common/ so ANY business layer (stream drivers, parsers, log
 * buffers, protocol decoders ...) can grab it by name. It is a plain OOC object
 * (fun + vtable, like the drv/ classes) but is NOT a `device` subclass — it is
 * just a byte FIFO, with zero chip knowledge, so it ports untouched.
 *
 * The data-stream device base class (iface/stream_device.h) holds a POINTER to
 * one of these (`rx_rb`); a driver that needs an RX ring attaches one via
 * stream_device_init_ringbuffer() (heap-allocated, backed by the driver's own
 * storage) and then push (ISR context) / pop (thread context) through the
 * ringbuffer API. Drivers that don't need a ring leave the pointer NULL.
 *
 * CONCURRENCY MODEL — single-producer / single-consumer (SPSC), lock-free:
 *   - the PRODUCER (e.g. a receive ISR) only ever writes `head`;
 *   - the CONSUMER (e.g. the read() path) only ever writes `tail`;
 *   - queries (available/free_space/is_empty/is_full) read both but tolerate a
 *     slightly stale snapshot, which is correct for SPSC.
 * This is exactly the UART pattern (one ISR fills, one thread drains), so no
 * mutex is needed and the class stays RTOS-agnostic. For multi-producer or
 * multi-consumer use, wrap the calls with your own lock.
 *
 * One slot is reserved to tell "full" from "empty" (head==tail), so the usable
 * capacity is `size - 1`. `size` must be >= 2.
 */
typedef struct _ringbuffer ringbuffer;

/* public method table (OOC "fun") — the API callers go through */
struct ringbufferFun {
    void (*destroy)(ringbuffer *self);
    void (*init)(ringbuffer *self);
    void (*deinit)(ringbuffer *self);
    int  (*put)(ringbuffer *self, uint8_t c);             /* 0 ok, -1 full (no-overwrite) */
    int  (*get)(ringbuffer *self, uint8_t *c);            /* 0 ok, -1 empty */
    size_t (*write)(ringbuffer *self, const void *data, size_t len); /* bytes stored */
    size_t (*read)(ringbuffer *self, void *data, size_t len);       /* bytes removed */
    int  (*peek)(ringbuffer *self, size_t index, uint8_t *c);       /* 0 ok, -1 out of range */
    size_t (*available)(ringbuffer *self);                /* bytes currently stored */
    size_t (*free_space)(ringbuffer *self);               /* bytes that can still be stored */
    int  (*is_empty)(ringbuffer *self);                   /* 1 if empty */
    int  (*is_full)(ringbuffer *self);                    /* 1 if full */
    void (*clear)(ringbuffer *self);                      /* drop everything */
    size_t (*capacity)(ringbuffer *self);                 /* usable capacity (size-1) */
    void (*set_overwrite)(ringbuffer *self, int enable);  /* 1 => overwrite oldest when full */
};

/* virtual dispatch table (OOC "vtable") — overridable by a subclass
 * (e.g. a guarded ring buffer that adds a mutex, or a DMA-backed variant). */
struct ringbufferVtable {
    int  (*put)(ringbuffer *self, uint8_t c);
    int  (*get)(ringbuffer *self, uint8_t *c);
    size_t (*write)(ringbuffer *self, const void *data, size_t len);
    size_t (*read)(ringbuffer *self, void *data, size_t len);
    int  (*peek)(ringbuffer *self, size_t index, uint8_t *c);
    size_t (*available)(ringbuffer *self);
    size_t (*free_space)(ringbuffer *self);
    int  (*is_empty)(ringbuffer *self);
    int  (*is_full)(ringbuffer *self);
    void (*clear)(ringbuffer *self);
    size_t (*capacity)(ringbuffer *self);
    void (*set_overwrite)(ringbuffer *self, int enable);
};

struct _ringbuffer {
    const struct ringbufferVtable *vtable;  /* shared per-class vtable (pointer) */
    const struct ringbufferFun *fun;        /* shared per-class method table */
    uint8_t *buf;          /* storage: external (owns_buf=0) or self-allocated */
    size_t size;           /* total slots (usable capacity = size - 1) */
    volatile size_t head;  /* next write index  — written ONLY by the producer */
    volatile size_t tail;  /* next read index   — written ONLY by the consumer */
    int overwrite;         /* 1 => put() overwrites the oldest byte when full */
    int owns_buf;          /* 1 => buf was allocated by create(), free on destroy */
};

/* Board / caller fills this. Pass buf == NULL to have the class allocate
 * `size` bytes internally (freed on destroy). overwrite selects the full
 * behaviour (0 = reject, 1 = overwrite oldest). */
typedef struct {
    uint8_t *buf;     /* external storage, or NULL to allocate internally */
    size_t size;      /* capacity in bytes (MUST be >= 2) */
    int overwrite;    /* 1 => overwrite oldest byte when full */
} ringbuffer_config_t;

ringbuffer *ringbuffer_create(const void *config);
void ringbuffer_destroy(ringbuffer *self);
void ringbuffer_init(ringbuffer *self);
void ringbuffer_deinit(ringbuffer *self);

/* On-board self-test for the ring buffer class itself. Exercises put/get,
 * write/read blocks, peek, full/empty detection, no-overwrite rejection and
 * overwrite mode. Returns 1 if every check passes, 0 otherwise. */
int ringbuffer_run_selftest(void);

extern const struct ringbufferFun ringbuffer_fun;

#endif /* RINGBUFFER_H */
