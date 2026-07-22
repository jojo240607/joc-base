#include "rtc.h"
#include "devmgr/device_manager.h"
#include <stdlib.h>
#include <string.h>

/* virtual implementations dispatched through the unified device vtable */
static int rtc_dev_open(device *self);
static int rtc_dev_close(device *self);
static int rtc_dev_read(device *self, void *buf, size_t len);
static int rtc_dev_write(device *self, const void *buf, size_t len);
static int rtc_dev_ioctl(device *self, int cmd, void *arg);

/* control-class vtable (referenced by rtc_init and the device vtable) */
static int rtc_control_command(control_device *self, int cmd, void *arg);
static int rtc_control_set(control_device *self, int param, const void *val);
static int rtc_control_get(control_device *self, int param, void *val);

/* internal helpers */
static int rtc_set_time(rtc *p, const rtc_time_t *t);
static int rtc_get_time(rtc *p, rtc_time_t *t);
static int rtc_set_date(rtc *p, const rtc_date_t *d);
static int rtc_get_date(rtc *p, rtc_date_t *d);
static uint8_t bcd2dec(uint8_t v);
static uint32_t rtc_tr_from_time(const rtc_time_t *t);
static void rtc_time_from_tr(uint32_t tr, rtc_time_t *t);
static uint32_t rtc_dr_from_date(const rtc_date_t *d);
static void rtc_date_from_dr(uint32_t dr, rtc_date_t *d);

static const struct control_deviceVtable rtc_control_vtable = {
    .command = rtc_control_command,
    .set     = rtc_control_set,
    .get     = rtc_control_get,
};

static const struct deviceVtable rtc_dev_vtable = {
    .open  = rtc_dev_open,
    .close = rtc_dev_close,
    .read  = rtc_dev_read,
    .write = rtc_dev_write,
    .ioctl = rtc_dev_ioctl,
};

device *rtc_create(const void *config)
{
    const rtc_config_t *c = (const rtc_config_t *)config;
    if (!c) return NULL;
    rtc *p = (rtc *)malloc(sizeof(rtc));
    if (!p) return NULL;
    memset(p, 0, sizeof(rtc));
    p->hal = rtc_hal_create(c->periph);
    if (!p->hal) { free(p); return NULL; }
    p->parent.parent.type   = DEVICE_TYPE_RTC;     /* driver sets its own class */
    p->parent.parent.name   = c->name;             /* driver sets its own name */
    p->parent.parent.vtable = &rtc_dev_vtable;     /* base device vtable */
    p->parent.vtable        = &rtc_control_vtable; /* control-class vtable */
    p->parent.parent.class  = DEVICE_CLASS_CONTROL;
    return (device *)p;
}

void rtc_destroy(rtc *self)
{
    if (!self) return;
    rtc_hal_destroy(self->hal);
    free(self);
}

/* --- BCD <-> decimal helpers --- */
static uint8_t bcd2dec(uint8_t v) { return (uint8_t)(((v >> 4) * 10U) + (v & 0xFU)); }

/* Compose RTC_TR from a 24h time (PM=0): HT/HU, MNT/MNU, ST/SU. */
static uint32_t rtc_tr_from_time(const rtc_time_t *t)
{
    uint8_t ht  = (uint8_t)(t->hour / 10U);
    uint8_t hu  = (uint8_t)(t->hour % 10U);
    uint8_t mnt = (uint8_t)(t->min  / 10U);
    uint8_t mnu = (uint8_t)(t->min  % 10U);
    uint8_t st  = (uint8_t)(t->sec  / 10U);
    uint8_t su  = (uint8_t)(t->sec  % 10U);
    return ((uint32_t)(ht  & 0x3U) << 20)
         | ((uint32_t)(hu  & 0xFU) << 16)
         | ((uint32_t)(mnt & 0x7U) << 12)
         | ((uint32_t)(mnu & 0xFU) << 8)
         | ((uint32_t)(st  & 0x7U) << 4)
         |  (uint32_t)(su  & 0xFU);
}
static void rtc_time_from_tr(uint32_t tr, rtc_time_t *t)
{
    uint8_t ht  = (uint8_t)((tr >> 20) & 0x3U);
    uint8_t hu  = (uint8_t)((tr >> 16) & 0xFU);
    uint8_t mnt = (uint8_t)((tr >> 12) & 0x7U);
    uint8_t mnu = (uint8_t)((tr >> 8)  & 0xFU);
    uint8_t st  = (uint8_t)((tr >> 4)  & 0x7U);
    uint8_t su  = (uint8_t)( tr        & 0xFU);
    t->hour = bcd2dec((uint8_t)((ht << 4) | hu));
    t->min  = bcd2dec((uint8_t)((mnt << 4) | mnu));
    t->sec  = bcd2dec((uint8_t)((st << 4) | su));
}

/* Compose RTC_DR: YT/YU, WDU, MT/MU, DT/DU. */
static uint32_t rtc_dr_from_date(const rtc_date_t *d)
{
    uint8_t y = (uint8_t)(d->year % 100U);
    uint8_t yt  = (uint8_t)(y / 10U);
    uint8_t yu  = (uint8_t)(y % 10U);
    uint8_t mt  = (uint8_t)(d->month / 10U);
    uint8_t mu  = (uint8_t)(d->month % 10U);
    uint8_t dt  = (uint8_t)(d->day   / 10U);
    uint8_t du  = (uint8_t)(d->day   % 10U);
    uint8_t wd  = (uint8_t)(d->wday & 0x7U);
    return ((uint32_t)(yt  & 0xFU) << 20)
         | ((uint32_t)(yu  & 0xFU) << 16)
         | ((uint32_t)(wd  & 0x7U) << 13)
         | ((uint32_t)(mt  & 0x1U) << 12)
         | ((uint32_t)(mu  & 0xFU) << 8)
         | ((uint32_t)(dt  & 0x3U) << 4)
         |  (uint32_t)(du  & 0xFU);
}
static void rtc_date_from_dr(uint32_t dr, rtc_date_t *d)
{
    uint8_t yt = (uint8_t)((dr >> 20) & 0xFU);
    uint8_t yu = (uint8_t)((dr >> 16) & 0xFU);
    uint8_t wd = (uint8_t)((dr >> 13) & 0x7U);
    uint8_t mt = (uint8_t)((dr >> 12) & 0x1U);
    uint8_t mu = (uint8_t)((dr >> 8)  & 0xFU);
    uint8_t dt = (uint8_t)((dr >> 4)  & 0x3U);
    uint8_t du = (uint8_t)( dr        & 0xFU);
    d->year = (uint16_t)(bcd2dec((uint8_t)((yt << 4) | yu)) + 2000U);
    d->wday = wd;
    d->month = bcd2dec((uint8_t)((mt << 4) | mu));
    d->day   = bcd2dec((uint8_t)((dt << 4) | du));
}

/* --- unified device-interface virtual implementations --- */
static int rtc_dev_open(device *self)
{
    rtc *p = (rtc *)self;
    rtc_hal_enable(p->hal);                       /* clock tree + backup unlock */

    /* Configure a 1 Hz tick from LSI (~32 kHz): async=127, sync=255. */
    rtc_hal_enter_init(p->hal);
    rtc_hal_set_prer(p->hal, 127U, 255U);
    rtc_hal_exit_init(p->hal);

    /* Seed a deterministic calendar so the BIST is reproducible on every run.
     * TR and DR must each be written in their OWN initialisation phase: on this
     * silicon writing two calendar registers in one init phase silently drops
     * the first (DR) write, so we never write both in a single init session. */
    rtc_hal_enter_init(p->hal);
    rtc_time_t seed_t = { 0, 0, 0 };
    rtc_hal_set_tr(p->hal, rtc_tr_from_time(&seed_t));
    rtc_hal_exit_init(p->hal);

    rtc_hal_enter_init(p->hal);
    rtc_date_t seed_d = { 2000, 1, 1, 1 };
    rtc_hal_set_dr(p->hal, rtc_dr_from_date(&seed_d));
    rtc_hal_exit_init(p->hal);
    return 0;
}

static int rtc_dev_close(device *self)
{
    (void)self;
    return 0;
}

static int rtc_dev_read(device *self, void *buf, size_t len)
{
    rtc *p = (rtc *)self;
    if (len < sizeof(rtc_time_t)) return -1;
    return rtc_get_time(p, (rtc_time_t *)buf);
}
static int rtc_dev_write(device *self, const void *buf, size_t len)
{
    rtc *p = (rtc *)self;
    if (len < sizeof(rtc_time_t)) return -1;
    return rtc_set_time(p, (const rtc_time_t *)buf);
}

/* --- helpers used by the control/ioctl surface --- */
static int rtc_set_time(rtc *p, const rtc_time_t *t)
{
    if (!p || !t) return -1;
    rtc_hal_enter_init(p->hal);
    rtc_hal_set_tr(p->hal, rtc_tr_from_time(t));
    rtc_hal_exit_init(p->hal);
    return 0;
}
static int rtc_get_time(rtc *p, rtc_time_t *t)
{
    if (!p || !t) return -1;
    rtc_time_from_tr(rtc_hal_get_tr(p->hal), t);
    return 0;
}
static int rtc_set_date(rtc *p, const rtc_date_t *d)
{
    if (!p || !d) return -1;
    rtc_hal_enter_init(p->hal);
    rtc_hal_set_dr(p->hal, rtc_dr_from_date(d));   /* write ONLY DR (isolate) */
    rtc_hal_exit_init(p->hal);
    return 0;
}
static int rtc_get_date(rtc *p, rtc_date_t *d)
{
    if (!p || !d) return -1;
    rtc_date_from_dr(rtc_hal_get_dr(p->hal), d);
    return 0;
}

/* --- control-class ops — the REAL implementations; the base deviceVtable
 *     forwards ioctl() here. set()/get() are unused (the ioctl command surface
 *     is richer), so they return -1. */
static int rtc_control_command(control_device *self, int cmd, void *arg)
{
    rtc *p = (rtc *)self;
    switch (cmd) {
    case RTC_IOCTL_SET_TIME:  return rtc_set_time(p, (const rtc_time_t *)arg);
    case RTC_IOCTL_GET_TIME:  return rtc_get_time(p, (rtc_time_t *)arg);
    case RTC_IOCTL_SET_DATE:  return rtc_set_date(p, (const rtc_date_t *)arg);
    case RTC_IOCTL_GET_DATE:  return rtc_get_date(p, (rtc_date_t *)arg);
    case RTC_IOCTL_GET_PRER: {
        if (!arg) return -1;
        rtc_prer_t *pr = (rtc_prer_t *)arg;
        rtc_hal_get_prer(p->hal, &pr->prediv_a, &pr->prediv_s);
        return 0;
    }
    case RTC_IOCTL_GET_BDCR: {
        if (!arg) return -1;
        *(uint32_t *)arg = rtc_hal_get_bdcr();
        return 0;
    }
    case RTC_IOCTL_GET_RAW_DR: {
        if (!arg) return -1;
        *(uint32_t *)arg = rtc_hal_get_dr(p->hal);
        return 0;
    }
    default:
        return -1;
    }
}
static int rtc_control_set(control_device *self, int param, const void *val)
    { (void)self; (void)param; (void)val; return -1; }
static int rtc_control_get(control_device *self, int param, void *val)
    { (void)self; (void)param; (void)val; return -1; }

static int rtc_dev_ioctl(device *self, int cmd, void *arg)
    { return rtc_control_command((control_device *)self, cmd, arg); }
