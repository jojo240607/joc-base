/*
 * ST7789 LCD 控制器驱动（FSMC 8080 并行接口，Bank1 NE1）。
 *
 * 访问：CPU 写 FSMC Bank1 内存映射窗口——命令区（A16=0）/数据区（A16=1）。
 * 固件侧直接 volatile 指针读写，无需 CS 操作（NE1 由 FSMC 硬件自动片选）。
 * 初始化/填充经 ioctl 走命令序列；显存由模拟器维护，验收在模拟器侧断言。
 */
#include "lcd_st7789.h"
#include "drv/fsmc.h"
#include "devmgr/device_manager.h"
#include "log/app_log.h"
#include <stdlib.h>
#include <string.h>

#define LCD_REG   (*(volatile uint16_t *)0x60000000u) /* A16=0：命令区 */
#define LCD_DATA  (*(volatile uint16_t *)0x60008000u) /* A16=1：数据区 */

typedef struct _lcd_st7789 {
    device parent;
    lcd_st7789_config_t cfg;
    device *fsmc;
} lcd_st7789;

static void lcd_cmd(lcd_st7789 *self, uint8_t c) { (void)self; LCD_REG = c; }
static void lcd_data(lcd_st7789 *self, uint16_t d) { (void)self; LCD_DATA = d; }

/* 命令 + 定长参数 */
static void lcd_cmd_params(lcd_st7789 *self, uint8_t c, const uint8_t *p, int n)
{
    lcd_cmd(self, c);
    for (int i = 0; i < n; i++) lcd_data(self, p[i]);
}

static int lcd_st7789_dev_open(device *self)
{
    lcd_st7789 *p = (lcd_st7789 *)self;
    if (p->fsmc->vtable->open(p->fsmc) != 0) {
        log_printf(app_log(), LOG_ERROR, "lcd", "[lcd] %s: fsmc 依赖 open 失败\n", p->cfg.name);
        return -1;
    }
    /* 配置 Bank1：16 位数据总线 8080 接口 + 写使能 + MBKEN */
    uint32_t bcr = (0x1u << 4) | (1u << 12); /* MWID=01（16 位）| WREN */
    if (p->fsmc->vtable->ioctl(p->fsmc, FSMC_IOCTL_SET_BCR, &bcr) != 0) return -1;
    if (p->fsmc->vtable->ioctl(p->fsmc, FSMC_IOCTL_BANK1_ENABLE, NULL) != 0) return -1;
    uint32_t btr = 0x00000002u | (0x7u << 8) | (0x7u << 16); /* ADDHLD/DATAST 时序 */
    if (p->fsmc->vtable->ioctl(p->fsmc, FSMC_IOCTL_SET_BTR, &btr) != 0) return -1;
    return 0;
}

static int lcd_st7789_dev_close(device *self)
{
    lcd_st7789 *p = (lcd_st7789 *)self;
    if (p->fsmc) p->fsmc->vtable->close(p->fsmc);
    return 0;
}

/* 初始化序列：软复位 → 退出睡眠 → 像素格式/扫描方向 → 反色/开显示 */
static void lcd_init(lcd_st7789 *self)
{
    lcd_cmd(self, 0x01);              /* SWRESET */
    lcd_cmd(self, 0x11);              /* SLPOUT */
    { uint8_t p[1] = { 0x55 }; lcd_cmd_params(self, 0x3A, p, 1); } /* COLMOD 16bpp */
    { uint8_t p[1] = { 0x00 }; lcd_cmd_params(self, 0x36, p, 1); } /* MADCTL */
    lcd_cmd(self, 0x21);              /* INVON */
    lcd_cmd(self, 0x29);              /* DISPON */
}

/* 读 ID：RDDID（0x04）后读数据区 */
static int lcd_read_id(lcd_st7789 *self, uint8_t *id)
{
    lcd_cmd(self, 0x04);
    *id = (uint8_t)(LCD_DATA & 0xFF);
    return 0;
}

/* 窗口填充：CASET/RASET 设窗口 + RAMWR 逐像素 */
static int lcd_fill(lcd_st7789 *self, const lcd_fill_t *f)
{
    if (f->x1 >= LCD_W || f->y1 >= LCD_H || f->x0 > f->x1 || f->y0 > f->y1) return -1;
    uint8_t ca[4] = { (uint8_t)(f->x0 >> 8), (uint8_t)f->x0, (uint8_t)(f->x1 >> 8), (uint8_t)f->x1 };
    uint8_t ra[4] = { (uint8_t)(f->y0 >> 8), (uint8_t)f->y0, (uint8_t)(f->y1 >> 8), (uint8_t)f->y1 };
    lcd_cmd_params(self, 0x2A, ca, 4); /* CASET */
    lcd_cmd_params(self, 0x2B, ra, 4); /* RASET */
    lcd_cmd(self, 0x2C);               /* RAMWR */
    uint32_t n = (uint32_t)(f->x1 - f->x0 + 1) * (f->y1 - f->y0 + 1);
    for (uint32_t i = 0; i < n; i++) lcd_data(self, f->color);
    return 0;
}

static int lcd_st7789_dev_ioctl(device *self, int cmd, void *arg)
{
    lcd_st7789 *p = (lcd_st7789 *)self;
    switch (cmd) {
    case LCD_IOCTL_INIT:
        lcd_init(p);
        return 0;
    case LCD_IOCTL_GET_ID: {
        if (!arg) return -1;
        return lcd_read_id(p, (uint8_t *)arg);
    }
    case LCD_IOCTL_FILL: {
        if (!arg) return -1;
        return lcd_fill(p, (const lcd_fill_t *)arg);
    }
    default:
        return -1;
    }
}

static const struct deviceVtable lcd_st7789_dev_vtable = {
    .open   = lcd_st7789_dev_open,
    .close  = lcd_st7789_dev_close,
    .read   = NULL,
    .write  = NULL,
    .ioctl  = lcd_st7789_dev_ioctl,
    .irq_id = NULL,
};

device *lcd_st7789_create(const void *config)
{
    const lcd_st7789_config_t *c = (const lcd_st7789_config_t *)config;
    lcd_st7789 *self = (lcd_st7789 *)malloc(sizeof(lcd_st7789));
    if (!self) return NULL;
    memset(self, 0, sizeof(lcd_st7789));
    self->parent.vtable = &lcd_st7789_dev_vtable;
    self->parent.type   = DEVICE_TYPE_GPIO;
    self->parent.class  = DEVICE_CLASS_CONTROL;
    self->parent.name   = c->name;
    self->cfg = *c;

    self->fsmc = device_manager_get(c->fsmc);
    if (!self->fsmc) {
        log_printf(app_log(), LOG_ERROR, "lcd", "[lcd] %s: 依赖 fsmc %s 缺失\n", c->name, c->fsmc);
        free(self);
        return NULL;
    }
    return (device *)self;
}
