#ifndef RTC_H
#define RTC_H

#include "iface/device.h"
#include "iface/control_device.h"   /* RTC IS-A control_device (parameter/state) */
#include "hal/stm32/rtc_hal.h"
#include <stdint.h>

/* device-level control commands for the RTC driver */
#define RTC_IOCTL_SET_TIME  0x60   /* arg: rtc_time_t*  (24h) */
#define RTC_IOCTL_GET_TIME  0x61   /* arg: rtc_time_t* */
#define RTC_IOCTL_SET_DATE  0x62   /* arg: rtc_date_t* */
#define RTC_IOCTL_GET_DATE  0x63   /* arg: rtc_date_t* */
#define RTC_IOCTL_GET_PRER  0x64   /* arg: rtc_prer_t* (prescaler readback) */
#define RTC_IOCTL_GET_BDCR  0x65   /* arg: uint32_t*  (RCC->BDCR) */
#define RTC_IOCTL_GET_RAW_DR 0x67  /* arg: uint32_t*  (raw RTC->DR) */

/*
 * Driver layer — generic STM32 RTC. Platform-independent: it keeps only BCD
 * encode/decode logic and delegates ALL register work to the HAL. Switching
 * chips = rewrite the HAL only. Implements the unified `device` interface as a
 * CONTROL-class device (parameter/state surface, no bulk transfer).
 */
typedef struct _rtc rtc;

struct _rtc {
    control_device parent;        /* unified interface — MUST be first member (IS-A control_device) */
    rtc_hal_handle_t *hal;
};

/* RTC time (24-hour). */
typedef struct {
    uint8_t hour;   /* 0..23 */
    uint8_t min;    /* 0..59 */
    uint8_t sec;    /* 0..59 */
} rtc_time_t;

/* RTC date. Only the low two year digits are stored by the hardware. */
typedef struct {
    uint16_t year;  /* 2000..2099 */
    uint8_t  month; /* 1..12 */
    uint8_t  day;   /* 1..31 */
    uint8_t  wday;  /* 1..7 (Mon..Sun) */
} rtc_date_t;

typedef struct {
    uint32_t prediv_a;  /* async prescaler */
    uint32_t prediv_s;  /* sync prescaler */
} rtc_prer_t;

device *rtc_create(const void *config);
void rtc_destroy(rtc *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; rtc_create() reads it. */
typedef struct {
    const char *name;    /* logical device name */
    void *periph;        /* RTC peripheral base (e.g. (void *)RTC) */
} rtc_config_t;

#endif /* RTC_H */
