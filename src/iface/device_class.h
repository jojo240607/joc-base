#ifndef DEVICE_CLASS_H
#define DEVICE_CLASS_H

/*
 * Convenience aggregator for the four driver-class base types.
 *
 * Each family now lives in its OWN header so every file stays focused:
 *   iface/stream_device.h   — UART/SPI/I2C/I2S/ADC/DAC/CAN/ETH/USB/LCD
 *   iface/block_device.h    — Flash/NOR/NAND/EEPROM/SD/eMMC
 *   iface/event_device.h    — keys/encoders/EXTI/timers/RTC/semaphore
 *   iface/control_device.h  — GPIO/PWM/clock/pinmux/watchdog/power/DAC
 *
 * Include this header ONLY when you genuinely need all four at once; otherwise
 * include the specific family header your driver/subsystem actually needs.
 */
#include "iface/stream_device.h"
#include "iface/block_device.h"
#include "iface/event_device.h"
#include "iface/control_device.h"

#endif /* DEVICE_CLASS_H */
