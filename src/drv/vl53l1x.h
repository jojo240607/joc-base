#ifndef VL53L1X_H
#define VL53L1X_H

#include "iface/device.h"
#include <stdint.h>

/* VL53L1X ToF 激光测距传感器（I2C 7 位地址 0x29，16 位寄存器地址高字节在前）。
 *
 * 关键寄存器：0x010F WHO_AM_I=0xEA、0x000F FIRMWARE_SYSTEM_STATUS(bit3=固件就绪)、
 * 0x0000 SOFT_RESET、0x0040 RANGE_START、0x0013 RESULT_INTERRUPT_STATUS(bit3=完成)、
 * 0x0096 RESULT_RANGE_MM（16 位小端）。模拟器 vperiph/i2c/vl53l1x.rs 同协议。
 */

#define VL53L1X_IOCTL_GET_WHO      0x01 /* arg: uint8_t*（0xEA） */
#define VL53L1X_IOCTL_START_RANGE  0x02 /* arg: NULL — 启动一次测距 */
#define VL53L1X_IOCTL_GET_DISTANCE 0x03 /* arg: uint16_t* mm */
#define VL53L1X_IOCTL_CLEAR_INT    0x04 /* arg: NULL — 清测距完成中断 */

#define VL53L1X_WHO_AM_I 0xEA

/* Driver-specific board config — filled by the board layer. */
typedef struct {
    const char *name;   /* logical device name, e.g. "vl53l1x" */
    const char *i2c;    /* I2C 总线设备（0x29 挂其上） */
} vl53l1x_config_t;

device *vl53l1x_create(const void *config);

#endif /* VL53L1X_H */
