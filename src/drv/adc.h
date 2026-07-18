#ifndef ADC_H
#define ADC_H

#include "iface/device.h"
#include "adc_hal.h"          /* opaque handle ONLY — no STM32 types reach the driver */
#include "pinmux_hal.h"       /* pinmux_pin_t (explicit pin tuple for the board) */
#include <stdint.h>

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
    device parent;                /* unified interface — MUST be first member */
    const struct adcFun *fun;
    adc_hal_handle_t *hal;        /* opaque — driver never dereferences it */
    uint32_t channel;            /* logical channel (0..N), kept as driver state */
    uint32_t vdda_mv;            /* supply voltage in mV (default 3300) */
    pinmux_pin_t ain;            /* cached analog input pin (port, pin, af=0) */
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
    /* Exact analog input pin to claim, supplied by the board. Using the
     * concrete (port, pin) (af = 0 for analog) avoids any name lookup and a
     * pin conflict is rejected before the analog GPIO register is touched. */
    pinmux_pin_t ain;       /* e.g. {PINMUX_PORT_A, 0, 0} for ADC1_IN0 */
} adc_config_t;

extern const struct adcFun adc_fun;

#endif /* ADC_H */
