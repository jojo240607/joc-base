#include "sdio.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* SD command indices */
#define SD_CMD0   0   /* GO_IDLE_STATE */
#define SD_CMD2   2   /* ALL_SEND_CID */
#define SD_CMD3   3   /* SEND_RELATIVE_ADDR */
#define SD_CMD7   7   /* SELECT/DESELECT_CARD */
#define SD_CMD8   8   /* SEND_IF_COND */
#define SD_CMD9   9   /* SEND_CSD */
#define SD_CMD12  12  /* STOP_TRANSMISSION */
#define SD_CMD13  13  /* SEND_STATUS */
#define SD_CMD16  16  /* SET_BLOCKLEN */
#define SD_CMD17  17  /* READ_SINGLE_BLOCK */
#define SD_CMD18  18  /* READ_MULTIPLE_BLOCK */
#define SD_CMD24  24  /* WRITE_BLOCK */
#define SD_CMD25  25  /* WRITE_MULTIPLE_BLOCK */
#define SD_CMD55  55  /* APP_CMD */
#define SD_ACMD41 41  /* SD_SEND_OP_COND (via CMD55 + CMD41) */

/* SD response flags */
#define R1_READY_FOR_DATA  0x100U
#define R1_CURRENT_STATE   0x1E00U

static int  sdio_dev_open(device *self);
static int  sdio_dev_close(device *self);
static int  sdio_dev_read(device *self, void *buf, size_t len);
static int  sdio_dev_write(device *self, const void *buf, size_t len);
static int  sdio_dev_ioctl(device *self, int cmd, void *arg);
static int  sdio_control_command(control_device *self, int cmd, void *arg);
static int  sdio_control_set(control_device *self, int param, const void *val);
static int  sdio_control_get(control_device *self, int param, void *val);
static int  sdio_card_init(sdio *p);
static int  sdio_cmd(sdio *p, uint32_t idx, uint32_t arg, uint32_t resp_type, uint32_t *resp);
static int  sdio_acmd(sdio *p, uint32_t idx, uint32_t arg, uint32_t resp_type, uint32_t *resp);
static int  sdio_read_data(sdio *p, uint8_t *buf, uint32_t blk_addr, uint32_t count);
static int  sdio_write_data(sdio *p, const uint8_t *buf, uint32_t blk_addr, uint32_t count);

static const struct control_deviceVtable sdio_control_vtable = {
    .command = sdio_control_command, .set = sdio_control_set, .get = sdio_control_get,
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
    #define RESOLVE(field, var) do { \
        if (!pinmux_hal_resolve(c->field, &sp, &spn, &saf)) { free(p); return NULL; } \
        p->var##_port = sp; p->var##_pin = spn; p->var##_af = saf; \
    } while(0)
    RESOLVE(ck_signal, ck);  RESOLVE(cmd_signal, cmd);
    RESOLVE(d0_signal, d0);  RESOLVE(d1_signal, d1);
    RESOLVE(d2_signal, d2);  RESOLVE(d3_signal, d3);
    #undef RESOLVE

    p->hal = sdio_hal_create(c->peripheral); if (!p->hal) { free(p); return NULL; }
    p->parent.parent.vtable = &sdio_dev_vtable;
    p->parent.vtable = &sdio_control_vtable;
    p->parent.parent.type = DEVICE_TYPE_SDIO;
    p->parent.parent.class = DEVICE_CLASS_CONTROL;
    p->parent.parent.name = c->name;
    return &p->parent.parent;
}
void sdio_destroy(sdio *self) { if (!self) return; sdio_hal_destroy(self->hal); free(self); }

static int sdio_dev_open(device *self)
{
    sdio *p = (sdio *)self;
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        const char *on = p->parent.parent.name;
        { pinmux_pin_cfg_t cfg; cfg.af = p->ck_af; cfg.mode = 2; cfg.otype = 0; cfg.speed = 3; cfg.pupd = 0;
          if (pm->fun->request(pm, p->ck_port, p->ck_pin, p->ck_af, on)) return -2;
          pm->fun->config(pm, p->ck_port, p->ck_pin, &cfg); }
        { pinmux_pin_cfg_t cfg; cfg.af = p->cmd_af; cfg.mode = 2; cfg.otype = 0; cfg.speed = 3; cfg.pupd = 0;
          if (pm->fun->request(pm, p->cmd_port, p->cmd_pin, p->cmd_af, on)) return -2;
          pm->fun->config(pm, p->cmd_port, p->cmd_pin, &cfg); }
        { pinmux_pin_cfg_t cfg; cfg.af = p->d0_af; cfg.mode = 2; cfg.otype = 0; cfg.speed = 3; cfg.pupd = 0;
          if (pm->fun->request(pm, p->d0_port, p->d0_pin, p->d0_af, on)) return -2;
          pm->fun->config(pm, p->d0_port, p->d0_pin, &cfg); }
        { pinmux_pin_cfg_t cfg; cfg.af = p->d1_af; cfg.mode = 2; cfg.otype = 0; cfg.speed = 3; cfg.pupd = 0;
          if (pm->fun->request(pm, p->d1_port, p->d1_pin, p->d1_af, on)) return -2;
          pm->fun->config(pm, p->d1_port, p->d1_pin, &cfg); }
        { pinmux_pin_cfg_t cfg; cfg.af = p->d2_af; cfg.mode = 2; cfg.otype = 0; cfg.speed = 3; cfg.pupd = 0;
          if (pm->fun->request(pm, p->d2_port, p->d2_pin, p->d2_af, on)) return -2;
          pm->fun->config(pm, p->d2_port, p->d2_pin, &cfg); }
        { pinmux_pin_cfg_t cfg; cfg.af = p->d3_af; cfg.mode = 2; cfg.otype = 0; cfg.speed = 3; cfg.pupd = 0;
          if (pm->fun->request(pm, p->d3_port, p->d3_pin, p->d3_af, on)) return -2;
          pm->fun->config(pm, p->d3_port, p->d3_pin, &cfg); }
    }
    sdio_hal_enable_clock(p->hal);
    sdio_hal_power_up(p->hal);
    sdio_hal_set_clock_div(p->hal, 118);  /* 48 MHz / (118+2) ≈ 400 kHz for init */
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
static int sdio_dev_read(device *self, void *buf, size_t len) { (void)self;(void)buf;(void)len; return -1; }
static int sdio_dev_write(device *self, const void *buf, size_t len) { (void)self;(void)buf;(void)len; return -1; }

/* ---- Low-level SD command helpers ---- */
static int sdio_cmd(sdio *p, uint32_t idx, uint32_t arg, uint32_t resp_type, uint32_t *resp)
    { return sdio_hal_cmd(p->hal, idx, arg, resp_type, resp); }

static int sdio_acmd(sdio *p, uint32_t idx, uint32_t arg, uint32_t resp_type, uint32_t *resp)
{
    /* APP_CMD prefix: CMD55 + arg=0 → then the actual ACMD */
    uint32_t r1;
    int r = sdio_hal_cmd(p->hal, SD_CMD55, 0, 1, &r1);
    if (r) return -1;
    return sdio_hal_cmd(p->hal, idx, arg, resp_type, resp);
}

/* ---- SD card initialization ---- */
static int sdio_card_init(sdio *p)
{
    uint32_t r1, r3, r7, resp[4];

    /* CMD0: go idle */
    if (sdio_cmd(p, SD_CMD0, 0, 0, NULL)) return -1;

    /* CMD8: check SDHC. Arg: 0x1AA = 2.7-3.6V, check pattern 0xAA.
     * If card responds with R7 and echo the pattern, it's SDHC/SDXC.
     * If CMD8 times out, the card is SDSC or MMC. */
    int is_sdhc = 0;
    if (sdio_cmd(p, SD_CMD8, 0x1AA, 1, &r7) == 0) {
        if ((r7 & 0xFF) == 0xAA) is_sdhc = 1;  /* voltage match */
    }

    /* ACMD41: initialize with HCS bit for SDHC */
    uint32_t acmd_arg = is_sdhc ? 0x40000000U : 0;  /* HCS = 1 for SDHC */
    int timeout = 1000;
    do {
        if (sdio_acmd(p, SD_ACMD41, acmd_arg, 1, &r1)) return -1;
        if (timeout-- <= 0) return -1;
    } while (!(r1 & 0x80000000U));  /* wait busy clear */

    if (is_sdhc) {
        /* CMD58: read OCR to confirm CCS bit */
        if (sdio_cmd(p, 58, 0, 1, &r3)) return -1;
        if (!(r3 & 0x40000000U)) is_sdhc = 0;  /* card is SDSC despite CMD8 */
    }

    /* CMD2: get CID (long response R2) */
    if (sdio_cmd(p, SD_CMD2, 0, 2, resp)) return -1;
    for (int i = 0; i < 4; i++) {
        p->card.cid[15 - i*4]     = (uint8_t)(resp[i] >> 24);
        p->card.cid[15 - i*4 - 1] = (uint8_t)(resp[i] >> 16);
        p->card.cid[15 - i*4 - 2] = (uint8_t)(resp[i] >> 8);
        p->card.cid[15 - i*4 - 3] = (uint8_t)(resp[i]);
    }

    /* CMD3: get RCA */
    if (sdio_cmd(p, SD_CMD3, 0, 1, &r1)) return -1;
    p->card.rca = (uint16_t)(r1 >> 16);

    /* CMD9: get CSD */
    if (sdio_cmd(p, SD_CMD9, ((uint32_t)p->card.rca) << 16, 2, resp)) return -1;
    for (int i = 0; i < 4; i++) {
        p->card.csd[15 - i*4]     = (uint8_t)(resp[i] >> 24);
        p->card.csd[15 - i*4 - 1] = (uint8_t)(resp[i] >> 16);
        p->card.csd[15 - i*4 - 2] = (uint8_t)(resp[i] >> 8);
        p->card.csd[15 - i*4 - 3] = (uint8_t)(resp[i]);
    }

    /* Parse CSD to get block length and card size */
    uint8_t csd_v = p->card.csd[0] >> 6;   /* CSD structure version */
    if (csd_v == 0) {  /* CSD v1.0 (SDSC) */
        uint32_t c_size  = ((p->card.csd[6] & 3) << 10) | ((uint32_t)p->card.csd[7] << 2) | (p->card.csd[8] >> 6);
        uint32_t c_mult  = ((p->card.csd[9] & 3) << 1) | (p->card.csd[10] >> 7);
        uint32_t blk_len = 1U << ((p->card.csd[5] & 0xF));
        p->card.block_len = 512;
        p->card.card_size = (c_size + 1) * (1U << (c_mult + 2)) * (blk_len / 512);
    } else {  /* CSD v2.0 (SDHC/SDXC) */
        uint32_t c_size = ((uint32_t)(p->card.csd[7] & 0x3F) << 16) | ((uint32_t)p->card.csd[8] << 8) | p->card.csd[9];
        p->card.block_len = 512;
        p->card.card_size = (c_size + 1) * 1024;  /* in 512-byte blocks */
    }
    p->card.card_type = is_sdhc ? 2 : 1;

    /* CMD7: select card */
    if (sdio_cmd(p, SD_CMD7, ((uint32_t)p->card.rca) << 16, 1, &r1)) return -1;

    /* CMD16: set block length to 512 */
    if (sdio_cmd(p, SD_CMD16, 512, 1, &r1)) return -1;

    /* Raise clock to faster speed */
    sdio_hal_set_clock_div(p->hal, 2);  /* 48 / (2+2) = 12 MHz */

    p->card.ready = 1;
    return 0;
}

/* ---- Block read/write (delegated to HAL) ---- */
static int sdio_read_data(sdio *p, uint8_t *buf, uint32_t blk_addr, uint32_t count)
{
    if (!p->card.ready || !buf) return -1;
    return sdio_hal_read_block(p->hal, buf, blk_addr, count, p->card.card_type == 2);
}
static int sdio_write_data(sdio *p, const uint8_t *buf, uint32_t blk_addr, uint32_t count)
{
    if (!p->card.ready || !buf) return -1;
    return sdio_hal_write_block(p->hal, buf, blk_addr, count, p->card.card_type == 2);
}

/* ---- IOCTL ---- */
static int sdio_control_command(control_device *self, int cmd, void *arg)
{
    sdio *p = (sdio *)self;
    switch (cmd) {
    case SDIO_IOCTL_INIT:
        return sdio_card_init(p);
    case SDIO_IOCTL_READ_BLOCK: {
        sdio_blk_t *b = (sdio_blk_t *)arg; if (!b) return -1;
        b->result = sdio_read_data(p, b->buf, b->block_addr, b->count);
        return b->result;
    }
    case SDIO_IOCTL_WRITE_BLOCK: {
        sdio_blk_t *b = (sdio_blk_t *)arg; if (!b) return -1;
        b->result = sdio_write_data(p, b->buf, b->block_addr, b->count);
        return b->result;
    }
    case SDIO_IOCTL_GET_INFO:
        if (arg) *(sdio_card_info_t *)arg = p->card;
        return 0;
    case SDIO_IOCTL_GET_POWER:
        if (arg) *(uint32_t *)arg = sdio_hal_get_power(p->hal);
        return 0;
    case SDIO_IOCTL_GET_CLKCR:
        if (arg) *(uint32_t *)arg = sdio_hal_get_clkcr(p->hal);
        return 0;
    default:
        return -1;
    }
}
static int sdio_control_set(control_device *self, int param, const void *val) { (void)self;(void)param;(void)val; return -1; }
static int sdio_control_get(control_device *self, int param, void *val) { (void)self;(void)param;(void)val; return -1; }
static int sdio_dev_ioctl(device *self, int cmd, void *arg) { return sdio_control_command((control_device *)self, cmd, arg); }
