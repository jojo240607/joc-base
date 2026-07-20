#ifndef EXTI_H
#define EXTI_H

#include "iface/event_device.h"   /* exti IS-A event_device (async notification) */
#include "exti_hal.h"              /* opaque HAL handle (driver never sees EXTI_TypeDef) */
#include <stdint.h>

/*
 * External Interrupt (EXTI) driver — an EVENT device.
 *
 * A GPIO pin can raise an interrupt on a rising/falling/both edge. The driver
 * claims the pin through the pinmux (as an input), routes its port to the EXTI
 * line via SYSCFG (HAL), selects the edge, and registers an ISR through the
 * platform-independent irq framework. Subscribers get a DEVICE_EVENT_IRQ
 * notification when the pin edge fires.
 *
 * COORDINATION WITH THE IRQ FRAMEWORK (shared lines): pins 5..9 share the
 * EXTI9_5 NVIC line (IRQ23) and pins 10..15 share EXTI15_10 (IRQ40). The
 * generic irq_manager already hosts MULTIPLE handlers on one line and
 * reference-counts the NVIC, so several exti devices on a shared line coexist.
 * Each ISR guards on its OWN EXTI->PR bit, so a sibling pin on the same line
 * cannot spuriously trigger it — the same pattern as the timer drivers.
 */
typedef struct _exti exti;

/* Board fills this as DATA. pin_signal is a plain GPIO name (e.g. "GPIOE_5");
 * edge is EXTI_EDGE_*; pupd is the input pull (0 none, 1 up, 2 down). */
typedef struct {
    const char *name;        /* logical device name (e.g. "exti0") */
    const char *pin_signal;  /* plain GPIO name, e.g. "GPIOE_5" */
    int      edge;           /* EXTI_EDGE_RISING / FALLING / BOTH */
    uint8_t  pupd;           /* input pull-up/down (0/1/2) */
} exti_config_t;

struct _exti {
    event_device parent;        /* IS-A event_device IS-A device */
    exti_hal_handle_t *hal;     /* opaque HAL handle */
    irq_id_t irq;               /* this pin's NVIC line (from HAL) */
    int      edge;              /* cached trigger edge */
    const char *pin_signal;     /* cached pin signal name */
    pinmux_port_t port;         /* resolved port for pinmux claim */
    uint8_t  pin;               /* resolved pin */
    uint8_t  af;                /* resolved af (0 for plain GPIO) */
    uint8_t  pupd;              /* cached input pull */
    volatile uint32_t count;    /* free-running interrupt counter (ISR increments) */
    device_event_cb_t cb;       /* subscribed IRQ callback (or NULL) */
    void *cb_ctx;               /* callback context */
};

/* uniform create signature (device *(*)(const void *)) for the board node list */
device *exti_create(const void *config);
void exti_destroy(exti *self);

/* ioctl commands (driver-specific) */
#define EXTI_IOCTL_GET_COUNT   0x30   /* arg = uint32_t* : interrupt count */
#define EXTI_IOCTL_TRIGGER     0x31   /* arg = NULL : software-trigger the line */

#endif /* EXTI_H */
