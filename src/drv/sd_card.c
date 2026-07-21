#include "sd_card.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SD_CMD0 0  /* GO_IDLE_STATE */
#define SD_CMD2 2  /* ALL_SEND_CID */
#define SD_CMD3 3  /* SEND_RELATIVE_ADDR */
#define SD_CMD7 7  /* SELECT/DESELECT_CARD */
#define SD_CMD8 8  /* SEND_IF_COND */
#define SD_CMD9 9  /* SEND_CSD */
#define SD_CMD12 12
#define SD_CMD16 16 /* SET_BLOCKLEN */
#define SD_CMD55 55

static int  sc_dev_open(device *self);
static int  sc_dev_close(device *self);
static int  sc_dev_read(device *self, void *buf, size_t len);
static int  sc_dev_write(device *self, const void *buf, size_t len);
static int  sc_dev_ioctl(device *self, int cmd, void *arg);
static int  sc_block_read(block_device *self, uint64_t lba, void *buf, uint32_t count);
static int  sc_block_write(block_device *self, uint64_t lba, const void *buf, uint32_t count);
static int  sc_block_erase(block_device *self, uint64_t lba, uint32_t count);
static int  sc_block_info(block_device *self, block_device_info_t *info);
static int  sc_init(sd_card *p);

static const struct block_deviceVtable sc_block_vtable = {
    .read = sc_block_read, .write = sc_block_write,
    .erase = sc_block_erase, .get_info = sc_block_info,
};
static const struct deviceVtable sc_dev_vtable = {
    .open = sc_dev_open, .close = sc_dev_close,
    .read = sc_dev_read, .write = sc_dev_write, .ioctl = sc_dev_ioctl,
};

device *sd_card_create(const void *config)
{
    const sd_card_config_t *c = (const sd_card_config_t *)config;
    if (!c) return NULL;
    sd_card *p = (sd_card *)malloc(sizeof(sd_card)); if (!p) return NULL;
    memset(p, 0, sizeof(sd_card));
    pinmux_port_t sp; uint8_t spn, saf;
    #define RS(s) if(!pinmux_hal_resolve(c->s,&sp,&spn,&saf)){free(p);return NULL;}
    RS(ck_signal);p->ck_port=sp;p->ck_pin=spn;p->ck_af=saf;
    RS(cmd_signal);p->cmd_port=sp;p->cmd_pin=spn;p->cmd_af=saf;
    RS(d0_signal);p->d0_port=sp;p->d0_pin=spn;p->d0_af=saf;
    RS(d1_signal);p->d1_port=sp;p->d1_pin=spn;p->d1_af=saf;
    RS(d2_signal);p->d2_port=sp;p->d2_pin=spn;p->d2_af=saf;
    RS(d3_signal);p->d3_port=sp;p->d3_pin=spn;p->d3_af=saf;
    #undef RS
    p->hal = sdio_hal_create(c->peripheral); if (!p->hal) { free(p); return NULL; }
    p->parent.parent.vtable = &sc_dev_vtable;
    p->parent.vtable = &sc_block_vtable;
    p->parent.parent.type = DEVICE_TYPE_SD_CARD;
    p->parent.parent.class = DEVICE_CLASS_BLOCK;
    p->parent.parent.name = c->name;
    return &p->parent.parent;
}
void sd_card_destroy(sd_card *self) { if (!self) return; sdio_hal_destroy(self->hal); free(self); }

static int claim_one(pinmux *pm, pinmux_port_t port, uint8_t pin, uint8_t af, const char *owner)
{
    if (pm->fun->request(pm, port, pin, af, owner)) return -1;
    pinmux_pin_cfg_t cx;
    cx.af = af; cx.mode = 2; cx.otype = 0; cx.speed = 3; cx.pupd = 0;
    pm->fun->config(pm, port, pin, &cx);
    return 0;
}
static int sc_dev_open(device *self)
{
    sd_card *p = (sd_card *)self;
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        const char *on = p->parent.parent.name;
        if (claim_one(pm,p->ck_port,p->ck_pin,p->ck_af,on)) return -2;
        if (claim_one(pm,p->cmd_port,p->cmd_pin,p->cmd_af,on)) return -2;
        if (claim_one(pm,p->d0_port,p->d0_pin,p->d0_af,on)) return -2;
        if (claim_one(pm,p->d1_port,p->d1_pin,p->d1_af,on)) return -2;
        if (claim_one(pm,p->d2_port,p->d2_pin,p->d2_af,on)) return -2;
        if (claim_one(pm,p->d3_port,p->d3_pin,p->d3_af,on)) return -2;
    }
    sdio_hal_enable_clock(p->hal);
    sdio_hal_power_up(p->hal);
    sdio_hal_set_clock_div(p->hal, 118);
    sdio_hal_set_bus_width(p->hal, 4);
    sdio_hal_enable_ck(p->hal, 1);
    return 0;
}
static int sc_dev_close(device *self)
{
    sd_card *p = (sd_card *)self;
    sdio_hal_enable_ck(p->hal, 0); sdio_hal_power_down(p->hal);
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}
static int sc_dev_read(device *s, void *b, size_t l) { (void)s;(void)b;(void)l;return -1; }
static int sc_dev_write(device *s, const void *b, size_t l) { (void)s;(void)b;(void)l;return -1; }

/* ---- Block vtable ---- */
static int sc_block_read(block_device *self, uint64_t lba, void *buf, uint32_t count)
{
    sd_card *p = (sd_card *)self;
    if (!p->ready || !buf || count == 0) return -1;
    uint32_t addr = (p->card_type == 2) ? (uint32_t)lba : (uint32_t)(lba * 512);
    return sdio_hal_read_block(p->hal, buf, addr, count, p->card_type == 2);
}
static int sc_block_write(block_device *self, uint64_t lba, const void *buf, uint32_t count)
{
    sd_card *p = (sd_card *)self;
    if (!p->ready || !buf || count == 0) return -1;
    uint32_t addr = (p->card_type == 2) ? (uint32_t)lba : (uint32_t)(lba * 512);
    return sdio_hal_write_block(p->hal, buf, addr, count, p->card_type == 2);
}
static int sc_block_erase(block_device *self, uint64_t lba, uint32_t count)
    { (void)self;(void)lba;(void)count; return -1; }
static int sc_block_info(block_device *self, block_device_info_t *info)
{
    sd_card *p = (sd_card *)self;
    if (!info) return -1;
    info->block_size = 512;
    info->block_count = p->card_size;
    info->total_bytes = (uint64_t)p->card_size * 512;
    return 0;
}

/* ---- SD protocol (card init, called from ioctl) ---- */
static int sc_init(sd_card *p)
{
    uint32_t r1, r7, resp[4];
    if (sdio_hal_cmd(p->hal, SD_CMD0, 0, 0, NULL)) return -1;
    int is_sdhc = 0;
    if (sdio_hal_cmd(p->hal, SD_CMD8, 0x1AA, 1, &r7) == 0 && (r7 & 0xFF) == 0xAA) is_sdhc = 1;
    uint32_t aarg = is_sdhc ? 0x40000000U : 0;
    int tmo = 1000;
    do { if (sdio_hal_cmd(p->hal, SD_CMD55, 0, 1, &r1)) return -1;
         if (sdio_hal_cmd(p->hal, 41, aarg, 1, &r1)) return -1;
         if (--tmo <= 0) return -1;
    } while (!(r1 & 0x80000000U));
    if (is_sdhc) { if (sdio_hal_cmd(p->hal, 58, 0, 1, &r1)) return -1; if (!(r1 & 0x40000000U)) is_sdhc = 0; }
    if (sdio_hal_cmd(p->hal, SD_CMD2, 0, 2, resp)) return -1;
    for (int i = 0; i < 4; i++) {
        p->cid[15-i*4]=(uint8_t)(resp[i]>>24);p->cid[15-i*4-1]=(uint8_t)(resp[i]>>16);
        p->cid[15-i*4-2]=(uint8_t)(resp[i]>>8);p->cid[15-i*4-3]=(uint8_t)(resp[i]);
    }
    if (sdio_hal_cmd(p->hal, SD_CMD3, 0, 1, &r1)) return -1;
    p->rca = (uint16_t)(r1 >> 16);
    if (sdio_hal_cmd(p->hal, SD_CMD9, ((uint32_t)p->rca)<<16, 2, resp)) return -1;
    for (int i = 0; i < 4; i++) {
        p->csd[15-i*4]=(uint8_t)(resp[i]>>24);p->csd[15-i*4-1]=(uint8_t)(resp[i]>>16);
        p->csd[15-i*4-2]=(uint8_t)(resp[i]>>8);p->csd[15-i*4-3]=(uint8_t)(resp[i]);
    }
    uint8_t cv = p->csd[0] >> 6;
    if (cv == 0) {
        uint32_t cs = ((p->csd[6]&3)<<10)|((uint32_t)p->csd[7]<<2)|(p->csd[8]>>6);
        uint32_t cm = ((p->csd[9]&3)<<1)|(p->csd[10]>>7);
        uint32_t bl = 1U << (p->csd[5] & 0xF);
        p->block_len = 512; p->card_size = (cs+1)*(1U<<(cm+2))*(bl/512);
    } else {
        uint32_t cs = ((uint32_t)(p->csd[7]&0x3F)<<16)|((uint32_t)p->csd[8]<<8)|p->csd[9];
        p->block_len = 512; p->card_size = (cs+1)*1024;
    }
    p->card_type = is_sdhc ? 2 : 1;
    if (sdio_hal_cmd(p->hal, SD_CMD7, ((uint32_t)p->rca)<<16, 1, &r1)) return -1;
    if (sdio_hal_cmd(p->hal, SD_CMD16, 512, 1, &r1)) return -1;
    sdio_hal_set_clock_div(p->hal, 2);
    p->ready = 1;
    return 0;
}

static int sc_dev_ioctl(device *self, int cmd, void *arg)
{
    sd_card *p = (sd_card *)self;
    if (cmd == 0x60) return sc_init(p);  /* CARD_IOCTL_INIT */
    return -1;
}
