#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include "iface/device.h"

/*
 * DEVICE MANAGER — the "device driver management" layer.
 *
 * A tiny, GENERIC name -> device* registry. It knows NOTHING about any
 * specific driver or HAL: it only stores and retrieves `device *` by name.
 * The application obtains devices purely by name (e.g.
 * device_manager_get("uart0")) and never sees the underlying peripheral or
 * HAL handle. This is the C equivalent of Zephyr's device_get_binding() /
 * RT-Thread's rt_device_find().
 *
 * Construction (which HAL handle + which driver) lives in the BOARD layer
 * (src/board/), which is the only place that knows both the HAL and the
 * device names. The manager itself stays 100% platform-independent.
 */
/* 6 base + 14 timers + systick + 2 pwm + 3 exti + i2c + spi + sdio + sd_card
 * + dac + rtc + rng + crc = 33 on this board; size with headroom for more.
 * 现板上节点共 57（14 定时器 + 2 原有 uart + 新增 uart2/3 + 4 gpio + spi0/1/2 +
 * i2c0/1/2 + pwm0/1/2/3/4 + 其余基础节点）。DEVICE_MANAGER_MAX 须 ≥ 节点数，
 * 且又不能过大——g_node_store/g_hnode_store 是 BSS 静态数组，F407 仅 128K 主 SRAM。
 * 发布版(SELFTEST=OFF)系统 .bss 仅约 14KB，开发版(SELFTEST=ON)约 109KB；新布局下
 * App RAM 下沉到系统堆之下(0x20004800 起)，g_app_slot 固定钉在 0x2001DC00，故 .bss
 * 是否越界取决于开发版 .bss 终点(约 0x2001B657)是否超过 App RAM 起点——取 60
 * （57+3 余量）在开发版仍落界内。见记忆 36069048：超过上限的节点会被静默丢弃，
 * 新增板级设备后务必回头核对本值（尤其开发版 RAM 占用）。 */
#define DEVICE_MANAGER_MAX 60

/* Register a device under `name` (overwrites if the name already exists). */
void device_manager_register(const char *name, device *dev);

/* Look up a device by name; returns NULL if not found. */
device *device_manager_get(const char *name);

/* Remove a previously registered device. */
void device_manager_unregister(const char *name);

/* Clear all entries (useful for tests / soft reboot). */
void device_manager_reset(void);

#endif /* DEVICE_MANAGER_H */
