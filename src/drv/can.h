#ifndef CAN_H
#define CAN_H

#include "iface/device.h"
#include "iface/stream_device.h"  /* can IS-A stream_device (frame stream) */
#include "can_hal.h"             /* opaque handle ONLY — no STM32 types reach the driver */
#include "pinmux_hal.h"           /* pinmux_port_t */
#include <stdint.h>
#include <stddef.h>

/* device-level control commands for the CAN driver */
#define CAN_IOCTL_SEND_FRAME  0x01   /* arg: can_frame_t*  (transmit one frame) */
#define CAN_IOCTL_RECV_FRAME  0x02   /* arg: can_frame_t*  (receive one frame) */
#define CAN_IOCTL_GET_MCR     0x03   /* arg: uint32_t* CAN_MCR */
#define CAN_IOCTL_GET_BTR     0x04   /* arg: uint32_t* CAN_BTR (LBKM/SILM/timing) */
#define CAN_IOCTL_GET_MSR     0x05   /* arg: uint32_t* CAN_MSR (INAK/SLAK) */
#define CAN_IOCTL_GET_ESR     0x06   /* arg: uint32_t* CAN_ESR */
#define CAN_IOCTL_GET_TSR     0x07   /* arg: uint32_t* CAN_TSR */
#define CAN_IOCTL_GET_RF0R    0x08   /* arg: uint32_t* CAN_RF0R */
#define CAN_IOCTL_GET_FA1R    0x09   /* arg: uint32_t* CAN_FA1R (filter active) */
#define CAN_IOCTL_GET_FMR     0x0A   /* arg: uint32_t* CAN_FMR (filter master) */

/*
 * Driver layer — generic bxCAN (STM32F4). Platform-independent: holds ONLY an
 * opaque `can_hal_handle_t *`. Implements the unified `device` interface as a
 * STREAM device (CAN frames in / out). Only POLL mode is supported.
 */
typedef struct _can can;

struct _can {
    stream_device parent;         /* unified interface — MUST be first member (IS-A stream_device) */
    can_hal_handle_t *hal;        /* opaque — driver never dereferences it */
    void *periph;                 /* CAN1 (the bxCAN master) — board layer only */
    uint32_t pclk_hz;             /* APB1 clock for baud-timing diagnostics */
    uint32_t presc;               /* baud-rate prescaler */
    uint32_t sjw;                 /* sync jump width (tq) */
    uint32_t bs1;                 /* time segment 1 (tq) */
    uint32_t bs2;                 /* time segment 2 (tq) */
    int loopback;                 /* 1 = loopback (self-test, no transceiver) */
    int silent;                   /* 1 = silent (Tx pin tri-stated) */
    uint32_t tx_id;               /* default TX id for plain stream writes */
    int remap;                    /* 1 = route CAN1 to PB8/PB9 (SYSCFG CAN_REMAP) */
    /* resolved pin geometry (claimed at open) */
    pinmux_port_t tx_port;  uint8_t tx_pin;  uint8_t tx_af;
    pinmux_port_t rx_port;  uint8_t rx_pin;  uint8_t rx_af;
};

device *can_create(const void *config);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; can_create() reads it. */
typedef struct {
    const char *name;          /* logical device name */
    void *periph;              /* CAN1 */
    uint32_t pclk_hz;          /* APB1 clock (e.g. 42000000) */
    uint32_t presc;            /* baud-rate prescaler */
    uint32_t sjw;              /* (re)sync jump width (tq) */
    uint32_t bs1;              /* time segment 1 (tq) */
    uint32_t bs2;              /* time segment 2 (tq) */
    const char *tx_signal;     /* CANx_TX */
    const char *rx_signal;     /* CANx_RX */
    int loopback;              /* 1 = loopback mode (self-test) */
    int silent;                /* 1 = silent mode (do not drive the bus) */
    uint32_t tx_id;            /* default TX id for plain stream writes */
    int remap;                 /* 1 = route CAN1 to PB8/PB9 (SYSCFG CAN_REMAP) */
} can_config_t;

#endif /* CAN_H */
