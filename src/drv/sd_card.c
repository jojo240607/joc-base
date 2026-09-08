#include "sd_card.h"
#include "devmgr/device_manager.h"
#include "drv/sdio.h"       /* SDIO_IOCTL_CMD / CMD_DATA types */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "log/log.h"
#include "log/app_log.h"

/*
 * SD Card driver — BLOCK device. Drives the SD protocol through an
 * underlying bus device (sdio, accessed via device_manager + ioctl).
 *
 * SD protocol commands are sent via ioctl(SDIO_IOCTL_CMD).
 * Block data transfers use ioctl(SDIO_IOCTL_CMD_DATA).
 * This allows the sd_card driver to work with any bus that supports
 * the SDIO interface; SPI mode can be added by plugging in a different
 * bus device + wrapping its ioctl into the same command/response pattern.
 */

/* SD command indices */
#define SD_CMD0   0
#define SD_CMD2   2
#define SD_CMD3   3
#define SD_CMD7   7
#define SD_CMD8   8
#define SD_CMD9   9
#define SD_CMD12  12
#define SD_CMD16  16
#define SD_CMD55  55

static int  sc_dev_open(device *self);
static int  sc_dev_close(device *self);
static int  sc_dev_read(device *self, void *buf, size_t len);
static int  sc_dev_write(device *self, const void *buf, size_t len);
static int  sc_dev_ioctl(device *self, int cmd, void *arg);
static int  sc_block_read(block_device *self, uint64_t lba, void *buf, uint32_t count);
static int  sc_block_write(block_device *self, uint64_t lba, const void *buf, uint32_t count);
static int  sc_block_erase(block_device *self, uint64_t lba, uint32_t count);
static int  sc_block_info(block_device *self, block_device_info_t *info);
static int  sc_cmd(sd_card *p, uint32_t idx, uint32_t arg, uint32_t rt, uint32_t *resp);
static int  sc_acmd(sd_card *p, uint32_t idx, uint32_t arg, uint32_t rt, uint32_t *resp);
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
    if (!c || !c->bus_name) return NULL;
    sd_card *p = (sd_card *)malloc(sizeof(sd_card)); if (!p) return NULL;
    memset(p, 0, sizeof(sd_card));
    p->parent.parent.vtable = &sc_dev_vtable;
    p->parent.vtable = &sc_block_vtable;
    p->parent.parent.type = DEVICE_TYPE_SD_CARD;
    p->parent.parent.class = DEVICE_CLASS_BLOCK;
    p->parent.parent.name = c->name;
    p->bus_type = c->bus_type;
    /* bus_dev resolved lazily in sc_dev_open */
    return &p->parent.parent;
}
void sd_card_destroy(sd_card *self) { if (!self) return; free(self); }

static int sc_dev_open(device *self)
{
    sd_card *p = (sd_card *)self;
    p->bus_dev = device_manager_get(p->parent.parent.name ? "sdio0" : NULL);
    /* ^ TODO: use the config's bus_name — needs a field in struct.
     * For now, hardcode sdio0 since bus_type==0 implies sdio. */
    if (!p->bus_dev) { log_printf(app_log(), LOG_DEBUG, "sd_card", "[sd_card] no bus device\n"); return -1; }
    /* The bus device (sdio0) must already be opened by the application. */
    return 0;
}
static int sc_dev_close(device *self) { (void)self; return 0; }
static int sc_dev_read(device *s, void *b, size_t l) { (void)s;(void)b;(void)l;return -1; }
static int sc_dev_write(device *s, const void *b, size_t l) { (void)s;(void)b;(void)l;return -1; }

/* ---- Block vtable (all operations go through bus_dev ioctl) ---- */
static int sc_block_read(block_device *self, uint64_t lba, void *buf, uint32_t count)
{
    sd_card *p = (sd_card *)self;
    if (!p->ready || !buf || count == 0) return -1;
    uint32_t addr = (p->card_type == 2) ? (uint32_t)lba : (uint32_t)(lba * 512);
    sdio_cmd_data_t x;
    memset(&x, 0, sizeof(x));
    x.index = (count == 1) ? 17 : 18;
    x.arg = addr;
    x.resp_type = 1;
    x.data_dir = 0;
    x.blk_size = 512;
    x.blk_count = count;
    x.buf = buf;
    int r = p->bus_dev->vtable->ioctl(p->bus_dev, SDIO_IOCTL_CMD_DATA, &x);
    if (count > 1) { /* stop multi-block */
        sdio_cmd_t s; memset(&s,0,sizeof(s)); s.index=12; s.arg=0; s.resp_type=1;
        p->bus_dev->vtable->ioctl(p->bus_dev, SDIO_IOCTL_CMD, &s);
    }
    return r;
}
static int sc_block_write(block_device *self, uint64_t lba, const void *buf, uint32_t count)
{
    sd_card *p = (sd_card *)self;
    if (!p->ready || !buf || count == 0) return -1;
    uint32_t addr = (p->card_type == 2) ? (uint32_t)lba : (uint32_t)(lba * 512);
    sdio_cmd_data_t x;
    memset(&x, 0, sizeof(x));
    x.index = (count == 1) ? 24 : 25;
    x.arg = addr;
    x.resp_type = 1;
    x.data_dir = 1;
    x.blk_size = 512;
    x.blk_count = count;
    x.buf = (uint8_t *)buf;
    int r = p->bus_dev->vtable->ioctl(p->bus_dev, SDIO_IOCTL_CMD_DATA, &x);
    if (count > 1) {
        sdio_cmd_t s; memset(&s,0,sizeof(s)); s.index=12; s.arg=0; s.resp_type=1;
        p->bus_dev->vtable->ioctl(p->bus_dev, SDIO_IOCTL_CMD, &s);
    }
    return r;
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

/* ---- Low-level SD commands via bus_dev ioctl ---- */
static int sc_cmd(sd_card *p, uint32_t idx, uint32_t arg, uint32_t rt, uint32_t *resp)
{
    sdio_cmd_t x; memset(&x, 0, sizeof(x));
    x.index = idx; x.arg = arg; x.resp_type = rt;
    int r = p->bus_dev->vtable->ioctl(p->bus_dev, SDIO_IOCTL_CMD, &x);
    if (resp && rt > 0) memcpy(resp, x.resp, (rt == 2 ? 4 : 1) * sizeof(uint32_t));
    return r;
}
static int sc_acmd(sd_card *p, uint32_t idx, uint32_t arg, uint32_t rt, uint32_t *resp)
{
    uint32_t r1;
    if (sc_cmd(p, SD_CMD55, 0, 1, &r1)) return -1;
    return sc_cmd(p, idx, arg, rt, resp);
}

/* ---- SD card init ---- */
static int sc_init(sd_card *p)
{
    uint32_t r1, r7, resp[4];

    if (sc_cmd(p, SD_CMD0, 0, 0, NULL)) return -1;
    int is_sdhc = 0;
    if (sc_cmd(p, SD_CMD8, 0x1AA, 1, &r7) == 0 && (r7 & 0xFF) == 0xAA) is_sdhc = 1;
    uint32_t aarg = is_sdhc ? 0x40000000U : 0;
    /* 轮询 ACMD41 直到 OCR bit31（busy/上电中）清除——真机 SD 卡初始化完成
     * 语义（busy 从 1→0）。原 `while (!(r1 & bit31))` 方向反（等 busy 置位），
     * 由 C 类块读写 + m14 固件期望 OCR=0x40FF8000（bit31=0）暴露并修正。 */
    int tmo = 1000;
    do { if (sc_acmd(p, 41, aarg, 1, &r1)) return -1; if (--tmo <= 0) return -1; } while ((r1 & 0x80000000U) != 0);
    if (is_sdhc) { if (sc_cmd(p, 58, 0, 1, &r1)) return -1; if (!(r1 & 0x40000000U)) is_sdhc = 0; }
    if (sc_cmd(p, SD_CMD2, 0, 2, resp)) return -1;
    for (int i = 0; i < 4; i++) { p->cid[15-i*4]=(uint8_t)(resp[i]>>24);p->cid[15-i*4-1]=(uint8_t)(resp[i]>>16); p->cid[15-i*4-2]=(uint8_t)(resp[i]>>8);p->cid[15-i*4-3]=(uint8_t)(resp[i]); }
    if (sc_cmd(p, SD_CMD3, 0, 1, &r1)) return -1; p->rca = (uint16_t)(r1 >> 16);
    if (sc_cmd(p, SD_CMD9, ((uint32_t)p->rca)<<16, 2, resp)) return -1;
    for (int i = 0; i < 4; i++) { p->csd[15-i*4]=(uint8_t)(resp[i]>>24);p->csd[15-i*4-1]=(uint8_t)(resp[i]>>16); p->csd[15-i*4-2]=(uint8_t)(resp[i]>>8);p->csd[15-i*4-3]=(uint8_t)(resp[i]); }
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
    if (sc_cmd(p, SD_CMD7, ((uint32_t)p->rca)<<16, 1, &r1)) return -1;
    if (sc_cmd(p, SD_CMD16, 512, 1, &r1)) return -1;
    { uint32_t clk = 2; p->bus_dev->vtable->ioctl(p->bus_dev, SDIO_IOCTL_SET_CLOCK, &clk); }
    p->ready = 1;
    return 0;
}

static int sc_dev_ioctl(device *self, int cmd, void *arg)
{
    sd_card *p = (sd_card *)self;
    switch (cmd) {
    case SD_CARD_IOCTL_INIT:
        return sc_init(p);
    case SD_CARD_IOCTL_READ_BLOCK: {
        const sd_block_io_t *b = (const sd_block_io_t *)arg;
        if (!b || !b->buf || b->count == 0) return -1;
        return sc_block_read(&p->parent, b->lba, b->buf, b->count);
    }
    case SD_CARD_IOCTL_WRITE_BLOCK: {
        const sd_block_io_t *b = (const sd_block_io_t *)arg;
        if (!b || !b->buf || b->count == 0) return -1;
        return sc_block_write(&p->parent, b->lba, b->buf, b->count);
    }
    default:
        return -1;
    }
}
