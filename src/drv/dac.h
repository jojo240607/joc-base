#ifndef DAC_H
#define DAC_H

#include "iface/device.h"
#include "iface/stream_device.h"  /* dac IS-A stream_device (output stream) */
#include "dac_hal.h"              /* opaque handle ONLY — no STM32 types reach the driver */
#include "drv/dma.h"              /* dma / dma_stream_t (DMA engine) + dma_req_id_t */
#include "hal/stm32/tim_hal.h"    /* tim_hal_handle_t (DAC trigger timer, e.g. TIM6) */
#include "pinmux_hal.h"           /* pinmux_port_t (resolved port for the pinmux claim) */
#include <stdint.h>

/* main-SRAM scratch for a DMA burst of DAC samples (DMA cannot touch CCM).
 * Sized for a comfortable burst; larger writes fall back to a malloc. */
#define DAC_DMA_BOUNCE 64

/* device-level control commands for the DAC driver */
#define DAC_IOCTL_SET_VALUE 0x60   /* arg: uint16_t* (12-bit value to output) */
#define DAC_IOCTL_GET_VALUE 0x61   /* arg: uint16_t* (read DOR register) */
#define DAC_IOCTL_GET_CR    0x62   /* arg: uint32_t* (read CR register) */

/*
 * Driver layer — generic DAC (STM32F4 DAC1, channels 1/2). Platform-independent:
 * it holds ONLY an opaque `dac_hal_handle_t *` and never references DAC_TypeDef.
 */
typedef struct _dac dac;

struct _dac {
    stream_device parent;         /* unified interface — MUST be first member */
    dac_hal_handle_t *hal;        /* opaque — driver never dereferences it */
    uint32_t channel;            /* 1 or 2 */
    const char *out_signal;      /* cached output signal name (e.g. "DAC1_OUT_PA4") */
    pinmux_port_t port;          /* resolved port for pinmux claim */
    uint8_t pin;                 /* resolved pin for pinmux claim */
    uint8_t af;                  /* resolved af (0 for analog) */
    /* DMA engine (STREAM_MODE_DMA). The DAC channel is hard-wired to one
     * specific DMA stream (DAC1->DMA1_Stream5); resolved once at open() and kept
     * reserved. The bounce buffer lives in main SRAM (malloc'd dac struct) — DMA
     * cannot touch CCM, so it is NOT safe to DMA straight from a caller buffer
     * that may live in CCM (e.g. a stack array). */
    dma_req_id_t  dma_req;        /* cached from config (for re-acquire on reopen) */
    dma          *dma_dev;        /* resolved dma controller (NULL if no route) */
    dma_stream_t *dma_str;        /* reserved stream for this DAC (M2P) */
    uint16_t      dma_bounce[DAC_DMA_BOUNCE];  /* main-SRAM sample scratch */
    /* Trigger timer for DAC+DMA. In static mode the DAC never raises a DMA
     * request, so a DMA burst must be clocked by a timer TRGO. The board wires a
     * basic timer (TIM6/TIM7 — the silicon's dedicated DAC triggers). NULL means
     * no trigger timer is available, so STREAM_MODE_DMA is refused. */
    void          *trig_tim;      /* cached peripheral (TIM6) from config */
    tim_hal_handle_t *trig_hal;   /* trigger-timer HAL handle (NULL if absent) */
    int           has_trigger;    /* 1 if a trigger timer was supplied */
};

device *dac_create(const void *config);
void dac_destroy(dac *self);

/* Driver-specific board config — defined HERE, filled by the board. */
typedef struct {
    const char *name;       /* logical device name */
    void *periph;           /* DAC (board layer only) */
    uint32_t channel;       /* 1 or 2 */
    /* Output signal name to claim, supplied by the board. The pinmux resolves
     * it (e.g. "DAC1_OUT_PA4" -> PA4, af=0, analog) to the exact pad, so a pin
     * conflict is rejected before the analog GPIO register is touched. */
    const char *out_signal; /* e.g. "DAC1_OUT_PA4" */
    /* Optional DMA request the DAC's data path is hard-wired to (e.g.
     * DMA_REQ_DAC1). 0 (DMA_REQ_NONE) means "no DMA for this DAC" (the driver
     * then refuses STREAM_MODE_DMA). */
    dma_req_id_t dma_req;
    /* Optional trigger timer (e.g. TIM6) that clocks the DAC when DMA is used.
     * DAC+DMA REQUIRES a trigger source (static mode never raises a DMA
     * request), so this must be supplied together with dma_req. NULL = none. */
    void *trig_tim;
} dac_config_t;

#endif /* DAC_H */
