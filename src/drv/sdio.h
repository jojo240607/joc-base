#ifndef SDIO_H
#define SDIO_H

#include "iface/stream_device.h"
#include "sdio_hal.h"
#include "drv/dma.h"              /* dma / dma_stream_t (DMA engine) + dma_req_id_t */
#include "pinmux_hal.h"
#include <stdint.h>

/*
 * SDIO driver — STREAM device for the STM32 SDIO host peripheral.
 *
 * This is a PURE INTERFACE driver: it owns the pins and the SDIO peripheral,
 * but does NOT implement SD protocol logic. Higher-level drivers (sd_card)
 * use sdio_hal directly or send raw commands through ioctl.
 *
 * TRANSFER ENGINE: POLL (FIFO polling) or DMA. The SDIO host has its own DMA
 * request, so a data block can be moved by the DMA controller instead of the
 * CPU draining/feeding the FIFO. The DMA-only state (the reserved stream
 * handle) lives in the per-engine sdio_dma_t, reached via `eng`; POLL pays
 * nothing. The per-block data buffer always comes from the caller (e.g. the
 * sd_card driver) and MUST be in main SRAM (DMA cannot touch CCM).
 */
typedef struct _sdio sdio;

/* Per-engine state for the DMA engine: the single SDIO DMA stream (the SDIO
 * host has ONE DMA request line; direction is set per transfer via DCTRL.DTDIR,
 * so the one stream is reconfigured read(P2M)/write(M2P) each call). NULL for
 * POLL. Allocated on SET_MODE(DMA), freed on close. */
typedef struct {
    dma *dma_dev;          /* resolved dma controller (dma2) */
    dma_stream_t *dma_s;   /* the one acquired SDIO DMA stream */
} sdio_dma_t;

typedef struct {
    const char *name;
    void *peripheral;
    const char *ck_signal;
    const char *cmd_signal;
    const char *d0_signal;
    const char *d1_signal;
    const char *d2_signal;
    const char *d3_signal;
    /* Logical DMA request id (DMA_REQ_SDIO). The driver resolves it to a
     * concrete (controller, stream, channel) via dma_hal_route(); 0 / DMA_REQ_NONE
     * means "no DMA" (driver refuses STREAM_MODE_DMA). */
    dma_req_id_t dma_req;
} sdio_config_t;

struct _sdio {
    stream_device parent;
    sdio_hal_handle_t *hal;
    pinmux_port_t ck_port, cmd_port, d0_port, d1_port, d2_port, d3_port;
    uint8_t  ck_pin, cmd_pin, d0_pin, d1_pin, d2_pin, d3_pin;
    uint8_t  ck_af, cmd_af, d0_af, d1_af, d2_af, d3_af;
    dma_req_id_t dma_req;   /* cached from config */
    void *eng;              /* per-engine: sdio_dma_t* in DMA mode, NULL in POLL */
};

device *sdio_create(const void *config);
void sdio_destroy(sdio *self);

/* ioctl */
#define SDIO_IOCTL_CMD         0x50   /* arg = sdio_cmd_t* (raw command, no data) */
#define SDIO_IOCTL_CMD_DATA    0x51   /* arg = sdio_cmd_data_t* (command + data block) */
#define SDIO_IOCTL_SET_CLOCK   0x52   /* arg = uint32_t* (clkdiv) */
#define SDIO_IOCTL_GET_POWER   0x54
#define SDIO_IOCTL_GET_CLKCR   0x55

/* Raw command (no data phase) */
typedef struct {
    uint32_t index;
    uint32_t arg;
    uint32_t resp_type;   /* 0=none, 1=short, 2=long */
    uint32_t resp[4];     /* OUT */
} sdio_cmd_t;

/* Command with data phase (block read/write) */
typedef struct {
    uint32_t index;       /* command index */
    uint32_t arg;         /* command argument */
    uint32_t resp_type;   /* 0=none, 1=short, 2=long */
    uint32_t resp[4];     /* OUT */
    uint32_t data_dir;    /* 0=read (card→host), 1=write (host→card) */
    uint32_t blk_size;    /* block size in bytes (e.g. 512) */
    uint32_t blk_count;   /* block count */
    uint8_t *buf;         /* data buffer */
    int      result;      /* OUT: 0 = success */
} sdio_cmd_data_t;

#endif /* SDIO_H */
