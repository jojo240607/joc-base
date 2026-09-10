#ifndef PMW3901_H
#define PMW3901_H

#include "iface/device.h"
#include <stdint.h>

/* PMW3901 光流传感器（SPI 7bit 寄存器地址协议；PixArt 光学流）。
 *
 * 寄存器：0x00 Product_ID=0x49、0x01 Revision、0x02 Motion(bit7=数据就绪)、
 * 0x03/0x04 Delta_X_L/H（16 位有符号 8.8 定点）、0x05/0x06 Delta_Y_L/H、
 * 0x07 SQUAL。模拟器 vperiph/spi/pmw3901.rs 按同一协议回送数据。
 */

#define PMW3901_IOCTL_GET_PRODUCT_ID 0x01 /* arg: uint8_t* */
#define PMW3901_IOCTL_GET_MOTION     0x02 /* arg: pmw3901_motion_t*（读一帧） */

#define PMW3901_PRODUCT_ID 0x49

/* 一帧光流观测（8.8 定点口径：1.0px = 0x0100） */
typedef struct {
    int16_t dx;        /* X 像素位移 */
    int16_t dy;        /* Y 像素位移 */
    uint8_t squal;     /* 表面质量 0..169 */
    uint8_t motion;    /* 0x02 寄存器原值（bit7=数据就绪） */
} pmw3901_motion_t;

/* Driver-specific board config — filled by the board layer. */
typedef struct {
    const char *name;   /* logical device name, e.g. "pmw3901" */
    const char *spi;    /* SPI 总线设备（与 bmi088 共享，不同 CS） */
    const char *cs;     /* 片选 GPIO 信号名 */
} pmw3901_config_t;

device *pmw3901_create(const void *config);

#endif /* PMW3901_H */
