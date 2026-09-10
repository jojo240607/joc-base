#ifndef LCD_ST7789_H
#define LCD_ST7789_H

#include "iface/device.h"
#include <stdint.h>

/* ST7789 LCD 控制器驱动（FSMC 8080 并行接口，Bank1 NE1，A16 接 RS）。
 *
 * 窗口地址（FSMC Bank1 64KB 窗口内）：
 *   - 命令区 0x6000_0000（A16=0，RS=0）—— 写命令字节
 *   - 数据区 0x6000_8000（A16=1，RS=1）—— 写参数/16bpp 像素，读 ID
 * 模拟器 vperiph/fsmc/st7789.rs 按同一窗口布局回送 RDDID=0x85 并维护显存。
 */

#define LCD_IOCTL_INIT    0x01 /* arg: NULL — 初始化序列（SWRESET/SLPOUT/COLMOD/INVON） */
#define LCD_IOCTL_GET_ID  0x02 /* arg: uint8_t* — RDDID 读回（ST7789=0x85） */
#define LCD_IOCTL_FILL    0x03 /* arg: lcd_fill_t* — 窗口填充 */

#define LCD_W 240
#define LCD_H 320

/* 窗口填充参数 */
typedef struct {
    uint16_t x0, y0, x1, y1; /* 含边界 */
    uint16_t color;          /* RGB565 */
} lcd_fill_t;

/* Driver-specific board config — filled by the board layer. */
typedef struct {
    const char *name;   /* logical device name, e.g. "lcd0" */
    const char *fsmc;   /* FSMC 控制器设备（配置 Bank1 NE1 8080 时序） */
} lcd_st7789_config_t;

device *lcd_st7789_create(const void *config);

#endif /* LCD_ST7789_H */
