#include "sdio.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SD_CMD0   0   /* GO_IDLE_STATE */
#define SD_CMD2   2   /* ALL_SEND_CID */
#define SD_CMD3   3   /* SEND_RELATIVE_ADDR */
#define SD_CMD7   7   /* SELECT/DESELECT_CARD */
#define SD_CMD8   8   /* SEND_IF_COND */
#define SD_CMD9   9   /* SEND_CSD */
#define SD_CMD12  12  /* STOP_TRANSMISSION */
#define SD_CMD16  16  /* SET_BLOCKLEN */
#define SD_CMD55  55  /* APP_CMD */

static int  sdio_dev_open(device *self);
static int  sdio_dev_close(device *self);
static int  sdio_dev_read(device *self, void *buf, size_t len);
static int  sdio_dev_write(device *self, const void *buf, size_t len);
static int  sdio_dev_ioctl(device *self, int cmd, void *arg);
static int  sdio_stream_read(stream_device *self, void *buf, size_t len);
static int  sdio_stream_write(stream_device *self, const void *buf, size_t len);
static int  sdio_stream_flush(stream_device *self);
static int  sdio_stream_read_frame(stream_device *self, void *b, size_t l, void *m);
static int  sdio_stream_write_frame(stream_device *self, const void *b, size_t l, const void *m);
static int  sdio_card_init(sdio *p);
static int  sdio_read_data(sdio *p, uint8_t *buf, uint32_t addr, uint32_t cnt);
static int  sdio_write_data(sdio *p, const uint8_t *buf, uint32_t addr, uint32_t cnt);

static const struct stream_deviceVtable sdio_stream_vtable = {
    .read = sdio_stream_read, .write = sdio_stream_write, .flush = sdio_stream_flush,
    .read_frame = sdio_stream_read_frame, .write_frame = sdio_stream_write_frame,
    .transfer_sync = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};
static const struct deviceVtable sdio_dev_vtable = {
    .open = sdio_dev_open, .close = sdio_dev_close,
    .read = sdio_dev_read, .write = sdio_dev_write, .ioctl = sdio_dev_ioctl,
};

device *sdio_create(const void *config)
{
    const sdio_config_t *c = (const sdio_config_t *)config;
    if (!c) return NULL;
    sdio *p = (sdio *)malloc(sizeof(sdio)); if (!p) return NULL;
    memset(p, 0, sizeof(sdio));
    pinmux_port_t sp; uint8_t spn, saf;
    if (!pinmux_hal_resolve(c->ck_signal, &sp, &spn, &saf)) { free(p); return NULL; }
    p->ck_port = sp; p->ck_pin = spn; p->ck_af = saf;
    if (!pinmux_hal_resolve(c->cmd_signal, &sp, &spn, &saf)) { free(p); return NULL; }
    p->cmd_port = sp; p->cmd_pin = spn; p->cmd_af = saf;
    if (!pinmux_hal_resolve(c->d0_signal, &sp, &spn, &saf)) { free(p); return NULL; }
    p->d0_port = sp; p->d0_pin = spn; p->d0_af = saf;
    if (!pinmux_hal_resolve(c->d1_signal, &sp, &spn, &saf)) { free(p); return NULL; }
    p->d1_port = sp; p->d1_pin = spn; p->d1_af = saf;
    if (!pinmux_hal_resolve(c->d2_signal, &sp, &spn, &saf)) { free(p); return NULL; }
    p->d2_port = sp; p->d2_pin = spn; p->d2_af = saf;
    if (!pinmux_hal_resolve(c->d3_signal, &sp, &spn, &saf)) { free(p); return NULL; }
    p->d3_port = sp; p->d3_pin = spn; p->d3_af = saf;
    p->hal = sdio_hal_create(c->peripheral); if (!p->hal) { free(p); return NULL; }
    p->parent.parent.vtable = &sdio_dev_vtable;
    p->parent.vtable = &sdio_stream_vtable;
    p->parent.parent.type = DEVICE_TYPE_SDIO;
    p->parent.parent.class = DEVICE_CLASS_STREAM;
    p->parent.parent.name = c->name;
    p->parent.mode = STREAM_MODE_POLL;
    return &p->parent.parent;
}
void sdio_destroy(sdio *self) { if (!self) return; sdio_hal_destroy(self->hal); free(self); }

static int sdio_dev_open(device *self)
{
    sdio *p = (sdio *)self;
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        const char *on = p->parent.parent.name;
        { pinmux_pin_cfg_t c; c.af = p->ck_af; c.mode = 2; c.otype = 0; c.speed = 3; c.pupd = 0;
          if (pm->fun->request(pm, p->ck_port, p->ck_pin, p->ck_af, on)) return -2;
          pm->fun->config(pm, p->ck_port, p->ck_pin, &c); }
        { pinmux_pin_cfg_t c; c.af = p->cmd_af; c.mode = 2; c.otype = 0; c.speed = 3; c.pupd = 0;
          if (pm->fun->request(pm, p->cmd_port, p->cmd_pin, p->cmd_af, on)) return -2;
          pm->fun->config(pm, p->cmd_port, p->cmd_pin, &c); }
        { pinmux_pin_cfg_t c; c.af = p->d0_af; c.mode = 2; c.otype = 0; c.speed = 3; c.pupd = 0;
          if (pm->fun->request(pm, p->d0_port, p->d0_pin, p->d0_af, on)) return -2;
          pm->fun->config(pm, p->d0_port, p->d0_pin, &c); }
        { pinmux_pin_cfg_t c; c.af = p->d1_af; c.mode = 2; c.otype = 0; c.speed = 3; c.pupd = 0;
          if (pm->fun->request(pm, p->d1_port, p->d1_pin, p->d1_af, on)) return -2;
          pm->fun->config(pm, p->d1_port, p->d1_pin, &c); }
        { pinmux_pin_cfg_t c; c.af = p->d2_af; c.mode = 2; c.otype = 0; c.speed = 3; c.pupd = 0;
          if (pm->fun->request(pm, p->d2_port, p->d2_pin, p->d2_af, on)) return -2;
          pm->fun->config(pm, p->d2_port, p->d2_pin, &c); }
        { pinmux_pin_cfg_t c; c.af = p->d3_af; c.mode = 2; c.otype = 0; c.speed = 3; c.pupd = 0;
          if (pm->fun->request(pm, p->d3_port, p->d3_pin, p->d3_af, on)) return -2;
          pm->fun->config(pm, p->d3_port, p->d3_pin, &c); }
    }
    sdio_hal_enable_clock(p->hal);
    sdio_hal_power_up(p->hal);
    sdio_hal_set_clock_div(p->hal, 118);
    sdio_hal_set_bus_width(p->hal, 4);
    sdio_hal_enable_ck(p->hal, 1);
    return 0;
}
static int sdio_dev_close(device *self)
{
    sdio *p = (sdio *)self;
    sdio_hal_enable_ck(p->hal, 0);
    sdio_hal_power_down(p->hal);
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}

/* stream vtable: read/write raw FIFO data (SD frame-level) */
static int sdio_stream_read(stream_device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }
static int sdio_stream_write(stream_device *self, const void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }
static int sdio_stream_flush(stream_device *self) { (void)self; return 0; }
static int sdio_stream_read_frame(stream_device *self, void *b, size_t l, void *m)
    { (void)self;(void)b;(void)l;(void)m; return -1; }
static int sdio_stream_write_frame(stream_device *self, const void *b, size_t l, const void *m)
    { (void)self;(void)b;(void)l;(void)m; return -1; }
static int sdio_dev_read(device *self, void *buf, size_t len)
    { return sdio_stream_read((stream_device *)self, buf, len); }
static int sdio_dev_write(device *self, const void *buf, size_t len)
    { return sdio_stream_write((stream_device *)self, buf, len); }

/* ---- SD protocol (card init + block I/O, via sdio_hal) ---- */
static int sdio_card_init(sdio *p)
{
    uint32_t r1, r7, resp[4];
    if (sdio_hal_cmd(p->hal, SD_CMD0, 0, 0, NULL)) return -1;
    int is_sdhc = 0;
    if (sdio_hal_cmd(p->hal, SD_CMD8, 0x1AA, 1, &r7) == 0 && (r7 & 0xFF) == 0xAA) is_sdhc = 1;
    uint32_t acmd_arg = is_sdhc ? 0x40000000U : 0;
    int timeout = 1000;
    do { if (sdio_hal_cmd(p->hal, SD_CMD55, 0, 1, &r1)) return -1;
         if (sdio_hal_cmd(p->hal, 41, acmd_arg, 1, &r1)) return -1;
         if (timeout-- <= 0) return -1;
    } while (!(r1 & 0x80000000U));
    if (is_sdhc) {
        if (sdio_hal_cmd(p->hal, 58, 0, 1, &r1)) return -1;
        if (!(r1 & 0x40000000U)) is_sdhc = 0;
    }
    if (sdio_hal_cmd(p->hal, SD_CMD2, 0, 2, resp)) return -1;
    for (int i = 0; i < 4; i++) {
        p->card.cid[15-i*4]=(uint8_t)(resp[i]>>24); p->card.cid[15-i*4-1]=(uint8_t)(resp[i]>>16);
        p->card.cid[15-i*4-2]=(uint8_t)(resp[i]>>8); p->card.cid[15-i*4-3]=(uint8_t)(resp[i]);
    }
    if (sdio_hal_cmd(p->hal, SD_CMD3, 0, 1, &r1)) return -1;
    p->card.rca = (uint16_t)(r1 >> 16);
    if (sdio_hal_cmd(p->hal, SD_CMD9, ((uint32_t)p->card.rca) << 16, 2, resp)) return -1;
    for (int i = 0; i < 4; i++) {
        p->card.csd[15-i*4]=(uint8_t)(resp[i]>>24); p->card.csd[15-i*4-1]=(uint8_t)(resp[i]>>16);
        p->card.csd[15-i*4-2]=(uint8_t)(resp[i]>>8); p->card.csd[15-i*4-3]=(uint8_t)(resp[i]);
    }
    uint8_t csd_v = p->card.csd[0] >> 6;
    if (csd_v == 0) {
        uint32_t cs = ((p->card.csd[6]&3)<<10)|((uint32_t)p->card.csd[7]<<2)|(p->card.csd[8]>>6);
        uint32_t cm = ((p->card.csd[9]&3)<<1)|(p->card.csd[10]>>7);
        uint32_t bl = 1U << (p->card.csd[5] & 0xF);
        p->card.block_len = 512; p->card.card_size = (cs+1)*(1U<<(cm+2))*(bl/512);
    } else {
        uint32_t cs = ((uint32_t)(p->card.csd[7]&0x3F)<<16)|((uint32_t)p->card.csd[8]<<8)|p->card.csd[9];
        p->card.block_len = 512; p->card.card_size = (cs+1)*1024;
    }
    p->card.card_type = is_sdhc ? 2 : 1;
    if (sdio_hal_cmd(p->hal, SD_CMD7, ((uint32_t)p->card.rca)<<16, 1, &r1)) return -1;
    if (sdio_hal_cmd(p->hal, SD_CMD16, 512, 1, &r1)) return -1;
    sdio_hal_set_clock_div(p->hal, 2);
    p->card.ready = 1;
    return 0;
}
static int sdio_read_data(sdio *p, uint8_t *buf, uint32_t blk_addr, uint32_t cnt)
    { if (!p->card.ready||!buf) return -1; return sdio_hal_read_block(p->hal,buf,blk_addr,cnt,p->card.card_type==2); }
static int sdio_write_data(sdio *p, const uint8_t *buf, uint32_t blk_addr, uint32_t cnt)
    { if (!p->card.ready||!buf) return -1; return sdio_hal_write_block(p->hal,buf,blk_addr,cnt,p->card.card_type==2); }

static int sdio_dev_ioctl(device *self, int cmd, void *arg)
{
    sdio *p = (sdio *)self;
    switch (cmd) {
    case SDIO_IOCTL_INIT: return sdio_card_init(p);
    case SDIO_IOCTL_READ_BLOCK: { sdio_blk_t *b = arg; if (!b) return -1; b->result = sdio_read_data(p,b->buf,b->block_addr,b->count); return b->result; }
    case SDIO_IOCTL_WRITE_BLOCK: { sdio_blk_t *b = arg; if (!b) return -1; b->result = sdio_write_data(p,b->buf,b->block_addr,b->count); return b->result; }
    case SDIO_IOCTL_GET_INFO: if (arg) *(sdio_card_info_t *)arg = p->card; return 0;
    case SDIO_IOCTL_GET_POWER: if (arg) *(uint32_t *)arg = sdio_hal_get_power(p->hal); return 0;
    case SDIO_IOCTL_GET_CLKCR: if (arg) *(uint32_t *)arg = sdio_hal_get_clkcr(p->hal); return 0;
    case STREAM_IOCTL_SET_MODE: { if (!arg) return -1; p->parent.mode = *(const stream_xfer_mode_t *)arg; return 0; }
    case STREAM_IOCTL_GET_MODE: { if (arg) *(stream_xfer_mode_t *)arg = p->parent.mode; return 0; }
    default: return -1;
    }
}
