#ifndef DSHOT_H
#define DSHOT_H

#include "iface/device.h"
#include <stdint.h>

/* DShot 电调数字协议发送驱动（GPIO bit-bang，DShot300）。
 *
 * 帧格式：16 位 MSB 先发 = (油门 11 位 << 1 | 遥测 1 位) << 4 | CRC4。
 * 行空闲 HIGH；每位置短 LOW 脉冲（1 位）或长 LOW 脉冲（0 位），位周期恒定。
 * 模拟器 ESC 虚拟外设按 LOW 脉宽 vs 位周期一半分类解码（比例制，速率无关）。
 */

#define DSHOT_IOCTL_SEND 0x01 /* arg: const uint16_t* 油门 0..1999 */

/* Driver-specific board config — filled by the board layer. */
typedef struct {
    const char *name;   /* logical device name, e.g. "dshot0" */
    const char *gpio;   /* 输出引脚信号名，e.g. "GPIOE_10"（行空闲 HIGH） */
} dshot_config_t;

device *dshot_create(const void *config);

#endif /* DSHOT_H */
