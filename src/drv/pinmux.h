#ifndef PINMUX_H
#define PINMUX_H

#include "iface/device.h"
#include "iface/control_device.h"  /* pinmux IS-A control_device (arbitrator/state) */
#include "pinmux_hal.h"         /* pinmux_port_t, pinmux_pin_cfg_t (no STM32 types) */
#include <stdint.h>

/* device-level control commands for the pinmux driver (passed to device_ioctl) */
#define PINMUX_IOCTL_REQUEST       0x10   /* arg: pinmux_req_t*        */
#define PINMUX_IOCTL_REQUEST_SIGNAL 0x11  /* arg: pinmux_signal_req_t* */
#define PINMUX_IOCTL_RELEASE       0x12   /* arg: pinmux_loc_t*        */
#define PINMUX_IOCTL_RELEASE_OWNER 0x13   /* arg: const char* owner    */
#define PINMUX_IOCTL_CONFIG        0x14   /* arg: pinmux_config_req_t* */
#define PINMUX_IOCTL_QUERY         0x15   /* arg: pinmux_query_t*      */
#define PINMUX_IOCTL_DUMP          0x16   /* arg: NULL                 */

/*
 * Driver layer — pin multiplexer / conflict arbitrator.
 *
 * A single instance owns the WHOLE chip's pin-ownership matrix. Every other
 * driver (or the board) must claim its pins here BEFORE touching the hardware,
 * so two configurations that would fight over the same physical pin are caught
 * at claim time instead of producing silent electrical interference.
 *
 * The driver is platform-independent: it keeps ONLY the runtime ownership
 * state and delegates silicon knowledge (signal->(port,pin,af) and register
 * programming) to hal/stm32/pinmux_hal. Implements the unified `device`.
 */
typedef struct _pinmux pinmux;

/* per-pin ownership record (runtime state, not silicon knowledge) */
typedef struct {
    uint8_t claimed;          /* 1 = a driver currently owns this pin */
    uint8_t af;               /* AF number the owner programmed (0 for GPIO/analog) */
    const char *owner;        /* logical device name that owns the pin */
} pinmux_pin_state_t;

struct pinmuxFun {
    void (*destroy)(pinmux *self);
    void (*init)(pinmux *self);
    void (*deinit)(pinmux *self);
    /* convenience methods — reachable ONLY via self->fun->xxx(self, ...) */
    int  (*request)(pinmux *self, pinmux_port_t port, uint8_t pin,
                    uint8_t af, const char *owner);
    int  (*request_signal)(pinmux *self, const char *signal, const char *owner);
    int  (*release)(pinmux *self, pinmux_port_t port, uint8_t pin);
    int  (*release_owner)(pinmux *self, const char *owner);
    int  (*config)(pinmux *self, pinmux_port_t port, uint8_t pin,
                   const pinmux_pin_cfg_t *cfg);
    int  (*query)(pinmux *self, pinmux_port_t port, uint8_t pin,
                  const char **owner, uint8_t *af);
    void (*dump)(pinmux *self);
};

struct _pinmux {
    control_device parent;        /* unified interface — MUST be first member (IS-A control_device) */
    const struct pinmuxFun *fun;
    pinmux_pin_state_t state[PINMUX_PORT_COUNT][16];  /* ownership matrix */
};

device *pinmux_create(const void *config);
void pinmux_destroy(pinmux *self);
void pinmux_init(pinmux *self);
void pinmux_deinit(pinmux *self);

/* Run a built-in self-test of the conflict-detection logic. Returns 1 if pass. */
int pinmux_run_selftest(pinmux *self);

/* ---- ioctl argument structs (used via device_ioctl / PINMUX_IOCTL_*) ---- */
typedef struct {
    pinmux_port_t port;
    uint8_t pin;
    uint8_t af;
    const char *owner;
} pinmux_req_t;

typedef struct {
    const char *signal;
    const char *owner;
} pinmux_signal_req_t;

typedef struct {
    pinmux_port_t port;
    uint8_t pin;
} pinmux_loc_t;

typedef struct {
    pinmux_port_t port;
    uint8_t pin;
    const pinmux_pin_cfg_t *cfg;
} pinmux_config_req_t;

typedef struct {
    pinmux_port_t port;
    uint8_t pin;
    const char **owner;
    uint8_t *af;
} pinmux_query_t;

/* Driver-specific board config — filled by the board as DATA. */
typedef struct {
    const char *name;       /* logical device name */
} pinmux_config_t;

extern const struct pinmuxFun pinmux_fun;

#endif /* PINMUX_H */
