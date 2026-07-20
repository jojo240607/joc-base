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
/* 6 base devices (pinmux/clk/uart/led/adc/temp) + 11 timers + systick = 18 on
 * this board; size with headroom for more. */
#define DEVICE_MANAGER_MAX 24

/* Register a device under `name` (overwrites if the name already exists). */
void device_manager_register(const char *name, device *dev);

/* Look up a device by name; returns NULL if not found. */
device *device_manager_get(const char *name);

/* Remove a previously registered device. */
void device_manager_unregister(const char *name);

/* Clear all entries (useful for tests / soft reboot). */
void device_manager_reset(void);

#endif /* DEVICE_MANAGER_H */
