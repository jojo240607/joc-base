#include "spi.h"
#include "devmgr/device_manager.h"   /* resolve the pinmux arbiter by name */
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include "pinmux_hal.h"               /* pinmux_port_t (resolved port for claim) */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* virtual implementations dispatched through the unified device vtable */
static int spi_dev_open(device *self);
static int spi_dev_close(device *self);
static int spi_dev_read(device *self, void *buf, size_t len);
static int spi_dev_write(device *self, const void *buf, size_t len);
static int spi_dev_ioctl(device *self, int cmd, void *arg);

/* control-class vtable (command/set/get) */
static int spi_control_command(control_device *self, int cmd, void *arg);
static int spi_control_set(control_device *self, int param, const void *val);
static int spi_control_get(control_device *self, int param, void *val);

static const struct control_deviceVtable spi_control_vtable = {
    .command = spi_control_command,
    .set     = spi_control_set,
    .get     = spi_control_get,
};

static const struct deviceVtable spi_dev_vtable = {
    .open  = spi_dev_open,
    .close = spi_dev_close,
    .read  = spi_dev_read,
    .write = spi_dev_write,
    .ioctl = spi_dev_ioctl,
};

device *spi_create(const void *config)
{
    const spi_config_t *c = (const spi_config_t *)config;
    if (!c || !c->sck_signal || !c->miso_signal || !c->mosi_signal)
        return NULL;

    spi *p = (spi *)malloc(sizeof(spi));
    if (!p) return NULL;
    memset(p, 0, sizeof(spi));

    /* Resolve all three AF signal names up front. */
    pinmux_port_t sp; uint8_t spn, saf;
    if (!pinmux_hal_resolve(c->sck_signal, &sp, &spn, &saf)) {
        printf("[spi] %s: unknown SCK signal \"%s\"\r\n", c->name, c->sck_signal);
        free(p); return NULL;
    }
    pinmux_port_t mp; uint8_t mpn, maf;
    if (!pinmux_hal_resolve(c->miso_signal, &mp, &mpn, &maf)) {
        printf("[spi] %s: unknown MISO signal \"%s\"\r\n", c->name, c->miso_signal);
        free(p); return NULL;
    }
    pinmux_port_t dp; uint8_t dpn, daf;
    if (!pinmux_hal_resolve(c->mosi_signal, &dp, &dpn, &daf)) {
        printf("[spi] %s: unknown MOSI signal \"%s\"\r\n", c->name, c->mosi_signal);
        free(p); return NULL;
    }
    if (saf != maf || saf != daf) {
        printf("[spi] %s: AF mismatch SCK=%u MISO=%u MOSI=%u\r\n",
               c->name, (unsigned)saf, (unsigned)maf, (unsigned)daf);
        free(p); return NULL;
    }

    p->hal = spi_hal_create(c->peripheral);
    if (!p->hal) { free(p); return NULL; }

    p->parent.parent.vtable = &spi_dev_vtable;
    p->parent.vtable        = &spi_control_vtable;
    p->parent.parent.type   = DEVICE_TYPE_SPI;
    p->parent.parent.class  = DEVICE_CLASS_CONTROL;   /* SPI IS-A control_device */
    p->parent.parent.name   = c->name;
    p->pclk_hz = c->pclk_hz;
    p->baud_hz = c->baud_hz;
    p->sck_port = sp; p->sck_pin = spn; p->sck_af = saf;
    p->miso_port = mp; p->miso_pin = mpn; p->miso_af = maf;
    p->mosi_port = dp; p->mosi_pin = dpn; p->mosi_af = daf;

    return &p->parent.parent;
}

void spi_destroy(spi *self)
{
    if (!self) return;
    spi_hal_destroy(self->hal);
    free(self);
}

static int spi_dev_open(device *self)
{
    spi *p = (spi *)self;

    /* Claim + program all three pins through the pinmux BEFORE touching SPI regs.
     * SPI pins: AF push-pull, very-high speed, no pull. */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        if (pm->fun->request(pm, p->sck_port, p->sck_pin, p->sck_af,
                             p->parent.parent.name) != 0) {
            printf("[spi] %s: SCK P%c%d CONFLICT\r\n", p->parent.parent.name,
                   'A' + (int)p->sck_port, (int)p->sck_pin); return -2;
        }
        if (pm->fun->request(pm, p->miso_port, p->miso_pin, p->miso_af,
                             p->parent.parent.name) != 0) {
            printf("[spi] %s: MISO P%c%d CONFLICT\r\n", p->parent.parent.name,
                   'A' + (int)p->miso_port, (int)p->miso_pin); return -2;
        }
        if (pm->fun->request(pm, p->mosi_port, p->mosi_pin, p->mosi_af,
                             p->parent.parent.name) != 0) {
            printf("[spi] %s: MOSI P%c%d CONFLICT\r\n", p->parent.parent.name,
                   'A' + (int)p->mosi_port, (int)p->mosi_pin); return -2;
        }
        pinmux_pin_cfg_t cfg = { .af = p->sck_af, .mode = 2, .otype = 0,
                                 .speed = 3, .pupd = 0 };
        pm->fun->config(pm, p->sck_port, p->sck_pin, &cfg);
        cfg.af = p->miso_af;
        cfg.pupd = 1;            /* MISO input needs pull-up to avoid floating */
        pm->fun->config(pm, p->miso_port, p->miso_pin, &cfg);
        cfg.af = p->mosi_af;
        cfg.pupd = 0;
        pm->fun->config(pm, p->mosi_port, p->mosi_pin, &cfg);
    }

    spi_hal_enable_clock(p->hal);
    spi_hal_config(p->hal, p->pclk_hz, p->baud_hz);   /* programs SPE=1 */
    return 0;
}

static int spi_dev_close(device *self)
{
    spi *p = (spi *)self;
    spi_hal_set_peripheral_enable(p->hal, 0);           /* disable SPI */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}

static int spi_dev_read(device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }
static int spi_dev_write(device *self, const void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }

static int spi_control_command(control_device *self, int cmd, void *arg)
{
    spi *p = (spi *)self;
    switch (cmd) {
    case SPI_IOCTL_XFER: {
        if (!arg) return -1;
        spi_xfer_t *x = (spi_xfer_t *)arg;
        return spi_hal_transfer(p->hal, x->tx_buf, x->rx_buf, x->len);
    }
    case SPI_IOCTL_GET_CR1:
        if (arg) *(uint32_t *)arg = spi_hal_get_cr1(p->hal);
        return 0;
    case SPI_IOCTL_GET_BSY:
        if (arg) *(int *)arg = spi_hal_is_busy(p->hal);
        return 0;
    default:
        return -1;
    }
}

static int spi_control_set(control_device *self, int param, const void *val)
    { (void)self; (void)param; (void)val; return -1; }
static int spi_control_get(control_device *self, int param, void *val)
    { (void)self; (void)param; (void)val; return -1; }
static int spi_dev_ioctl(device *self, int cmd, void *arg)
    { return spi_control_command((control_device *)self, cmd, arg); }
