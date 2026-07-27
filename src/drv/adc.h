#ifndef ADC_H
#define ADC_H

#include "iface/device.h"
#include "iface/stream_device.h"  /* adc IS-A stream_device (sampling stream) */
#include "adc_hal.h"          /* opaque handle ONLY — no STM32 types reach the driver */
#include "drv/dma.h"          /* dma / dma_stream_t (DMA engine) + dma_req_id_t */
#include "pinmux_hal.h"       /* pinmux_port_t (resolved port for the pinmux claim) */
#include "osal/osal.h"        /* osal_sem_t (EOC completion) */
#include "irq.h"              /* irq_id_t (cached ADC IRQ id) */
#include <stdint.h>

/* main-SRAM scratch for a DMA burst of ADC samples (DMA cannot touch CCM).
 * Sized for a comfortable burst; larger reads fall back to a malloc. */
#define ADC_DMA_BOUNCE 64

/* device-level control commands for the ADC driver (passed to device_ioctl) */
#define ADC_IOCTL_SET_CHANNEL  0x01   /* arg: const uint32_t* channel */
#define ADC_IOCTL_GET_CHANNEL  0x02   /* arg: uint32_t* channel */
#define ADC_IOCTL_SET_VREF_MV  0x03   /* arg: const uint32_t* vdda_mv */
#define ADC_IOCTL_READ_MV      0x04   /* arg: uint32_t* mv (VDDA-scaled) */

/*
 * Driver layer — generic ADC. Platform-independent: it holds ONLY an opaque
 * `adc_hal_handle_t *` and never references ADC_TypeDef or any chip register.
 * Switching chips = rewrite hal/<new-platform>/adc_hal only; this file is
 * untouched. It implements the unified `device` interface (embeds `device
 * parent` as the first member and fills the vtable).
 */
typedef struct _adc adc;

struct adcFun {
    void (*destroy)(adc *self);
    void (*init)(adc *self);
    void (*deinit)(adc *self);
    /* typed convenience methods — reachable ONLY via self->fun->xxx(self),
       never as standalone functions (the concrete impls are `static` in .c) */
    uint32_t (*read)(adc *self);          /* single conversion, raw 12-bit */
    uint32_t (*read_mv)(adc *self);       /* converted to millivolts */
    void     (*set_channel)(adc *self, uint32_t channel);
};

struct _adc {
    stream_device parent;         /* unified interface — MUST be first member (IS-A stream_device) */
    const struct adcFun *fun;
    adc_hal_handle_t *hal;        /* opaque — driver never dereferences it */
    uint32_t channel;            /* logical channel (0..N), kept as driver state */
    uint32_t vdda_mv;            /* supply voltage in mV (default 3300) */
    const char *ain_signal;      /* cached analog-input signal name (e.g. "ADC1_IN0") */
    pinmux_port_t port;          /* resolved port for pinmux claim */
    uint8_t pin;                 /* resolved pin for pinmux claim */
    uint8_t af;                  /* resolved af (0 for analog) */
    /* IRQ-mode read state. In STREAM_MODE_IRQ the EOC ISR writes last_raw and
     * gives eoc_sem; adc_stream_read() blocks on eoc_sem. Unused in POLL mode. */
    volatile uint32_t last_raw;  /* last conversion result (written by EOC ISR) */
    osal_sem_t eoc_sem;          /* signaled by the EOC ISR (IRQ-mode read) */
    irq_id_t  eoc_irq;           /* cached ADC IRQ id (from adc_hal_irq_id) */
    /* DMA engine (STREAM_MODE_DMA). The ADC is hard-wired to one specific DMA
     * stream (ADC1->DMA2_Stream0); resolved once at open() and kept reserved.
     * The bounce buffer lives in main SRAM (malloc'd adc struct) — DMA cannot
     * touch CCM, so it is NOT safe to DMA straight into a caller buffer that may
     * live in CCM (e.g. a stack array). */
    dma_req_id_t  dma_req;        /* cached from config (for re-acquire on reopen) */
    dma          *dma_dev;        /* resolved dma controller (NULL if no route) */
    dma_stream_t *dma_str;        /* reserved stream for this ADC (P2M) */
    uint16_t      dma_bounce[ADC_DMA_BOUNCE];  /* main-SRAM sample scratch */
};

/* The board fills adc_config_t (defined below) as DATA and passes it in; the
 * driver therefore needs zero knowledge of which chip the handle wraps. The
 * create fn has the UNIFORM signature  device *(*)(const void *config)  so the
 * board can list it directly as a node — no per-driver build wrapper needed. */
device *adc_create(const void *config);
void adc_destroy(adc *self);
void adc_init(adc *self);
void adc_deinit(adc *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; adc_create() reads it. */
typedef struct {
    const char *name;       /* logical device name */
    void *periph;           /* ADC1 (board layer only) */
    uint32_t channel;       /* default / logical channel */
    uint32_t vdda_mv;       /* supply voltage in mV */
    /* Analog-input signal name to claim, supplied by the board. The pinmux
     * resolves it (e.g. "ADC1_IN0" -> PA0, af=0) to the exact pad, so a pin
     * conflict is rejected before the analog GPIO register is touched. Leave
     * NULL for internal channels (16/17/18) that need no GPIO pin. */
    const char *ain_signal; /* e.g. "ADC1_IN0" */
    /* Optional DMA request the ADC's data path is hard-wired to (e.g.
     * DMA_REQ_ADC1). 0 (DMA_REQ_NONE) means "no DMA for this ADC" (the driver
     * then refuses STREAM_MODE_DMA). */
    dma_req_id_t dma_req;
} adc_config_t;

extern const struct adcFun adc_fun;

#endif /* ADC_H */
