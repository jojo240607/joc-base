#include "pinmux.h"
#include "pinmux_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* virtual implementations dispatched through the unified device vtable */
static int pinmux_dev_open(device *self);
static int pinmux_dev_close(device *self);
static int pinmux_dev_read(device *self, void *buf, size_t len);
static int pinmux_dev_write(device *self, const void *buf, size_t len);
static int pinmux_dev_ioctl(device *self, int cmd, void *arg);

/* public methods — `static`, reachable ONLY through self->fun-> */
static int  pinmux_request(pinmux *self, pinmux_port_t port, uint8_t pin,
                           uint8_t af, const char *owner);
static int  pinmux_request_signal(pinmux *self, const char *signal, const char *owner);
static int  pinmux_release(pinmux *self, pinmux_port_t port, uint8_t pin);
static int  pinmux_release_owner(pinmux *self, const char *owner);
static int  pinmux_config(pinmux *self, pinmux_port_t port, uint8_t pin,
                          const pinmux_pin_cfg_t *cfg);
static int  pinmux_query(pinmux *self, pinmux_port_t port, uint8_t pin,
                         const char **owner, uint8_t *af);
static void pinmux_dump(pinmux *self);

const struct pinmuxFun pinmux_fun = {
    .destroy        = pinmux_destroy,
    .init           = pinmux_init,
    .deinit         = pinmux_deinit,
    .request        = pinmux_request,
    .request_signal = pinmux_request_signal,
    .release        = pinmux_release,
    .release_owner  = pinmux_release_owner,
    .config         = pinmux_config,
    .query          = pinmux_query,
    .dump           = pinmux_dump,
};

/* one shared vtable for the whole pinmux class — assigned by pinmux_init() */
static const struct deviceVtable pinmux_dev_vtable = {
    .open  = pinmux_dev_open,
    .close = pinmux_dev_close,
    .read  = pinmux_dev_read,
    .write = pinmux_dev_write,
    .ioctl = pinmux_dev_ioctl,
};

/* Uniform create signature for the board layer: takes ONLY the driver's own
 * config pointer and returns a device *. The board lists this fn directly as a
 * node — no per-driver build wrapper. */
device *pinmux_create(const void *config)
{
    const pinmux_config_t *c = (const pinmux_config_t *)config;
    pinmux *self = (pinmux *)malloc(sizeof(pinmux));
    if (!self) return NULL;
    memset(self, 0, sizeof(pinmux));
    self->parent.type = DEVICE_TYPE_PINMUX;          /* driver sets its own class */
    self->parent.name = (c && c->name) ? c->name : "pinmux";
    pinmux_init(self);
    return (device *)self;
}

void pinmux_destroy(pinmux *self)
{
    if (!self) return;
    pinmux_deinit(self);
    free(self);
}

void pinmux_init(pinmux *self)
{
    if (!self) return;
    self->parent.vtable = &pinmux_dev_vtable;   /* per-class shared vtable */
    self->fun = &pinmux_fun;
    /* ownership matrix is already zeroed by the create() memset */
}

void pinmux_deinit(pinmux *self)
{
    if (!self) return;
    /* no base vtable to free (it is per-class static const) */
}

/* --- core: claim a pin, rejecting conflicts -------------------------------
 * Returns:  0  = claimed (or idempotent re-claim by the same owner)
 *          -1  = bad argument (NULL owner / out of range)
 *          -2  = CONFLICT: pin already owned by a different owner or function
 */
static int pinmux_request(pinmux *self, pinmux_port_t port, uint8_t pin,
                          uint8_t af, const char *owner)
{
    if (!self || !owner) return -1;
    if (port >= PINMUX_PORT_COUNT || pin >= 16U) return -1;

    pinmux_pin_state_t *s = &self->state[port][pin];
    if (s->claimed) {
        /* allowed only if the SAME owner re-claims the SAME function */
        if (s->af == af && s->owner && strcmp(s->owner, owner) == 0)
            return 0;                       /* idempotent — OK */
        return -2;                          /* -EBUSY: conflict */
    }
    s->claimed = 1;
    s->af = af;
    s->owner = owner;
    return 0;
}

static int pinmux_request_signal(pinmux *self, const char *signal, const char *owner)
{
    pinmux_port_t port;
    uint8_t pin, af;
    if (!pinmux_hal_resolve(signal, &port, &pin, &af))
        return -3;                          /* -ENOENT: unknown signal name */
    return pinmux_request(self, port, pin, af, owner);
}

static int pinmux_release(pinmux *self, pinmux_port_t port, uint8_t pin)
{
    if (!self || port >= PINMUX_PORT_COUNT || pin >= 16U) return -1;
    pinmux_pin_state_t *s = &self->state[port][pin];
    if (!s->claimed) return -4;             /* not claimed */
    s->claimed = 0;
    s->owner = NULL;
    return 0;
}

static int pinmux_release_owner(pinmux *self, const char *owner)
{
    if (!self || !owner) return -1;
    int freed = 0;
    for (int p = 0; p < PINMUX_PORT_COUNT; p++)
        for (int n = 0; n < 16; n++) {
            pinmux_pin_state_t *s = &self->state[p][n];
            if (s->claimed && s->owner && strcmp(s->owner, owner) == 0) {
                s->claimed = 0;
                s->owner = NULL;
                freed++;
            }
        }
    return freed;                            /* number of pins released */
}

static int pinmux_config(pinmux *self, pinmux_port_t port, uint8_t pin,
                         const pinmux_pin_cfg_t *cfg)
{
    (void)self;
    if (!cfg) return -1;
    if (port >= PINMUX_PORT_COUNT || pin >= 16U) return -1;
    pinmux_hal_config(port, pin, cfg);       /* program the GPIO registers */
    return 0;
}

static int pinmux_query(pinmux *self, pinmux_port_t port, uint8_t pin,
                        const char **owner, uint8_t *af)
{
    if (!self || port >= PINMUX_PORT_COUNT || pin >= 16U) return -1;
    pinmux_pin_state_t *s = &self->state[port][pin];
    if (!s->claimed) return -4;              /* not claimed */
    if (owner) *owner = s->owner;
    if (af) *af = s->af;
    return 0;
}

static void pinmux_dump(pinmux *self)
{
    if (!self) return;
    static const char letters[] = "ABCDEFGHI";
    printf("pinmux claims:\r\n");
    for (int p = 0; p < PINMUX_PORT_COUNT; p++) {
        for (int n = 0; n < 16; n++) {
            pinmux_pin_state_t *s = &self->state[p][n];
            if (!s->claimed) continue;
            const char *sig = pinmux_hal_signal_at((pinmux_port_t)p,
                                                   (uint8_t)n, s->af);
            char gpio_buf[12];
            if (!sig && s->af == 0) {       /* plain GPIO: no DB row, derive name */
                snprintf(gpio_buf, sizeof(gpio_buf), "GPIO%c%d",
                         letters[p], n);
                sig = gpio_buf;
            }
            if (sig)
                printf("  P%c%d af%d owner=%s [%s]\r\n",
                       letters[p], n, s->af,
                       s->owner ? s->owner : "?", sig);
            else
                printf("  P%c%d af%d owner=%s\r\n",
                       letters[p], n, s->af,
                       s->owner ? s->owner : "?");
        }
    }
}

/* --- unified device-interface virtual implementations --- */

static int pinmux_dev_open(device *self)
{
    (void)self;
    /* No hardware to bring up: the pinmux is a pure bookkeeping arbitrator.
     * Pins are claimed (and optionally configured) on demand via request(). */
    return 0;
}

static int pinmux_dev_close(device *self)
{
    (void)self;
    return 0;
}

static int pinmux_dev_read(device *self, void *buf, size_t len)
{
    (void)self; (void)buf; (void)len;
    return -1;                              /* not a data stream */
}

static int pinmux_dev_write(device *self, const void *buf, size_t len)
{
    (void)self; (void)buf; (void)len;
    return -1;                              /* not a data stream */
}

static int pinmux_dev_ioctl(device *self, int cmd, void *arg)
{
    pinmux *pm = (pinmux *)self;
    if (!pm || !arg) return -1;
    switch (cmd) {
    case PINMUX_IOCTL_REQUEST: {
        pinmux_req_t *r = (pinmux_req_t *)arg;
        return pinmux_request(pm, r->port, r->pin, r->af, r->owner);
    }
    case PINMUX_IOCTL_REQUEST_SIGNAL: {
        pinmux_signal_req_t *r = (pinmux_signal_req_t *)arg;
        return pinmux_request_signal(pm, r->signal, r->owner);
    }
    case PINMUX_IOCTL_RELEASE: {
        pinmux_loc_t *r = (pinmux_loc_t *)arg;
        return pinmux_release(pm, r->port, r->pin);
    }
    case PINMUX_IOCTL_RELEASE_OWNER:
        return pinmux_release_owner(pm, (const char *)arg);
    case PINMUX_IOCTL_CONFIG: {
        pinmux_config_req_t *r = (pinmux_config_req_t *)arg;
        return pinmux_config(pm, r->port, r->pin, r->cfg);
    }
    case PINMUX_IOCTL_QUERY: {
        pinmux_query_t *r = (pinmux_query_t *)arg;
        return pinmux_query(pm, r->port, r->pin, r->owner, r->af);
    }
    case PINMUX_IOCTL_DUMP:
        pinmux_dump(pm);
        return 0;
    default:
        return -1;
    }
}

/* --- built-in self-test of the conflict-detection logic ------------------ */
int pinmux_run_selftest(pinmux *self)
{
    if (!self) return 0;
    int pass = 1;
    int r;

    printf("\r\n--- pinmux self-test (conflict detection) ---\r\n");

    /* 1) a fresh claim must succeed */
    r = pinmux_request(self, PINMUX_PORT_B, 6, 4, "i2c_test");   /* PB6 = I2C1_SCL */
    printf("[pinmux] claim PB6/I2C1_SCL : %s\r\n", r == 0 ? "OK" : "FAIL");
    pass &= (r == 0);

    /* 2) a DIFFERENT owner grabbing the same pin must be REJECTED (-2) */
    r = pinmux_request(self, PINMUX_PORT_B, 6, 4, "spurious");
    printf("[pinmux] conflict on PB6     : %s\r\n", r == -2 ? "DETECTED" : "MISSED");
    pass &= (r == -2);

    /* 3) the SAME owner re-claiming the SAME function is idempotent (0) */
    r = pinmux_request(self, PINMUX_PORT_B, 6, 4, "i2c_test");
    printf("[pinmux] re-claim PB6        : %s\r\n", r == 0 ? "OK" : "FAIL");
    pass &= (r == 0);

    /* 4) after release it can be claimed again */
    pinmux_release(self, PINMUX_PORT_B, 6);
    r = pinmux_request(self, PINMUX_PORT_B, 6, 4, "i2c_test");
    printf("[pinmux] re-claim after free : %s\r\n", r == 0 ? "OK" : "FAIL");
    pass &= (r == 0);
    pinmux_release(self, PINMUX_PORT_B, 6);   /* leave it free for the board */

    /* 5) claim the board's REAL USART1 pins by SIGNAL NAME — this is how the
     *    uart driver claims them now. The unique names ("USART1_TX_PA9" vs
     *    "USART1_TX_PB6") resolve to exactly one pad, so there is no ambiguity. */
    r = pinmux_request_signal(self, "USART1_TX_PA9", "uart0");   /* PA9  = USART1_TX */
    printf("[pinmux] claim USART1_TX_PA9  : %s\r\n", r == 0 ? "OK" : "FAIL");
    pass &= (r == 0);
    r = pinmux_request_signal(self, "USART1_RX_PA10", "uart0");  /* PA10 = USART1_RX */
    pass &= (r == 0);

    /* 5b) the SAME peripheral's ALTERNATE pin (PB6 = USART1_TX on a different
     *     pad) is independently claimable under its OWN unique name. This proves
     *     the duplicate-name problem is gone: both physical locations are
     *     reachable by distinct names. */
    r = pinmux_request_signal(self, "USART1_TX_PB6", "uart0");   /* PB6 = USART1_TX alt */
    printf("[pinmux] alt name USART1_TX_PB6: %s\r\n", r == 0 ? "OK" : "FAIL");
    pass &= (r == 0);
    pinmux_release(self, PINMUX_PORT_B, 6);                   /* free it again */

    /* 6) an unknown signal name must still be rejected (-3) via request_signal */
    r = pinmux_request_signal(self, "NOPE_NOPE", "x");
    printf("[pinmux] unknown signal      : %s\r\n", r == -3 ? "REJECTED" : "MISSED");
    pass &= (r == -3);

    printf("PINMUX SELF-TEST: %s\r\n", pass ? "PASS" : "FAIL");
    return pass;
}
