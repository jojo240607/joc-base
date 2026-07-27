#ifndef DMA_H
#define DMA_H

#include "iface/device.h"
#include "iface/control_device.h"  /* dma IS-A control_device (resource manager) */
#include "dma_hal.h"               /* opaque handle ONLY — no STM32 types reach the driver */
#include "osal/osal.h"             /* osal_sem_t (transfer-complete signaling) */
#include <stdint.h>

/* device-level control commands for the DMA driver (passed to device_ioctl) */
#define DMA_IOCTL_FREE_STREAMS  0x01   /* arg: uint32_t* -> number of free streams */

/*
 * Driver layer — generic DMA controller. Platform-independent: it holds ONLY an
 * array of opaque `dma_hal_stream_t *` handles and never references DMA_TypeDef
 * or any chip register. Switching chips = rewrite hal/<new-platform>/dma_hal
 * only; this file is untouched. It implements the unified `device` interface
 * (embeds `control_device parent` as the first member and fills the vtable).
 *
 * A DMA controller is modeled as a CONTROL device that manages a POOL of 8
 * streams. Other drivers obtain it by name ("dma1"/"dma2") via the device
 * manager, then drive it through `self->fun->...` (the typed methods below) —
 * exactly how adc.c drives pinmux via pm->fun->request().
 */
typedef enum {
    DMA_DIR_P2M = 0,   /* peripheral -> memory (DIR=00) */
    DMA_DIR_M2P = 1,   /* memory -> peripheral (DIR=01) */
    DMA_DIR_M2M = 2,   /* memory -> memory (DIR=10) — runs on EN, no req needed */
} dma_dir_t;

typedef enum {
    DMA_DATA_8  = 0,
    DMA_DATA_16 = 1,
    DMA_DATA_32 = 2,
} dma_data_size_t;

typedef enum {
    DMA_PRIO_LOW       = 0,
    DMA_PRIO_MED       = 1,
    DMA_PRIO_HIGH      = 2,
    DMA_PRIO_VERYHIGH  = 3,
} dma_prio_t;

/* Opaque stream handle returned by acquire() and handed back to config/start/
 * wait/free. The caller MUST NOT dereference it. Defined here (not just
 * forward-declared) because struct _dma embeds an array of them. */
struct dma_stream { int idx; };
typedef struct dma_stream dma_stream_t;

typedef struct _dma dma;

struct dmaFun {
    void (*destroy)(dma *self);
    void (*init)(dma *self);
    void (*deinit)(dma *self);
    /* acquire a stream for a transfer. `stream_idx` is the concrete stream to
     * use (0..7) — for a peripheral request this MUST be the value from
     * dma_hal_route(); pass DMA_STREAM_ANY to let the driver pick a free stream
     * (used by memory-to-memory). `channel` is the CHSEL value (0..7, again from
     * dma_hal_route() for peripherals, or 0 for M2M). `dir` selects P2M/M2P/M2M.
     * Returns NULL if the requested stream is busy or out of range. The stream
     * stays reserved until free() is called. */
    dma_stream_t *(*acquire)(dma *self, uint8_t stream_idx, uint8_t channel, dma_dir_t dir);
    /* program a previously-acquired stream: periph address (PAR), memory address
     * (M0AR), item count, unit size, address increments, priority. The stream is
     * left DISABLED (call start() to arm it). Returns 0 on success. */
    int (*config)(dma *self, dma_stream_t *s, const void *periph, void *mem,
                  uint32_t count, dma_data_size_t size,
                  int periph_inc, int mem_inc, dma_prio_t prio);
    /* arm the stream (EN=1) with an optional completion callback (cb may be
     * NULL). The callback runs in interrupt context when TC (or TE) fires.
     * Returns 0 on success. */
    int (*start)(dma *self, dma_stream_t *s, void (*cb)(void *), void *ctx);
    /* block until the transfer completes (or errors). Returns 0 when done. */
    int (*wait_done)(dma *self, dma_stream_t *s, uint32_t timeout_ms);
    /* non-blocking completion check (1 = done, 0 = still running). */
    int (*poll_done)(dma *self, dma_stream_t *s);
    /* items still pending in NDTR. */
    uint32_t (*remaining)(dma *self, dma_stream_t *s);
    /* release a stream back to the pool. */
    void (*free)(dma *self, dma_stream_t *s);
};

#define DMA_STREAMS_PER_CTLR 8
/* Sentinel for acquire(): pick ANY free stream (used by memory-to-memory
 * transfers, where the silicon imposes no fixed stream). Peripheral requests
 * MUST pass the concrete stream index from dma_hal_route() instead. */
#define DMA_STREAM_ANY 0xFF

/* Per-stream runtime state (one entry per pool stream). `hal` / `owner` are
 * back-pointers so the per-stream ISR can recover its context without a global
 * search. */
typedef struct {
    int               in_use;
    dma_dir_t         dir;
    uint32_t          channel;
    struct dma_hal_stream *hal;   /* HAL handle for this stream (for the ISR) */
    struct _dma      *owner;      /* owning controller (for the ISR) */
    osal_sem_t        done_sem;   /* given by the TC/TE ISR, taken by wait_done */
    void            (*cb)(void *);
    void             *cb_ctx;
} dma_stream_rt_t;

struct _dma {
    control_device parent;        /* unified interface — MUST be first member */
    const struct dmaFun *fun;
    /* pool state (indexed 0..7); opaque to callers. */
    struct dma_hal_stream *hal[DMA_STREAMS_PER_CTLR];
    dma_stream_rt_t        streams[DMA_STREAMS_PER_CTLR];
    struct dma_stream      handles[DMA_STREAMS_PER_CTLR]; /* one opaque handle per stream */
};

device *dma_create(const void *config);
void dma_destroy(dma *self);
void dma_init(dma *self);
void dma_deinit(dma *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; dma_create() reads it. */
typedef struct {
    const char *name;       /* logical device name ("dma1"/"dma2") */
    void *periph;           /* DMA1 / DMA2 (board layer only) */
} dma_config_t;

extern const struct dmaFun dma_fun;

#endif /* DMA_H */
