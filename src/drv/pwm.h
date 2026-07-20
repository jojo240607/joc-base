#ifndef PWM_H
#define PWM_H

#include "iface/control_device.h"   /* pwm IS-A control_device (command/set/get) */
#include "tim_hal.h"                /* opaque HAL handle (driver never sees TIM_TypeDef) */
#include "pinmux_hal.h"             /* pinmux_port_t (resolved port for the pinmux claim) */
#include <stdint.h>

/*
 * PWM driver — a CONTROL device built on a TIM capture/compare channel.
 *
 * A GP TIM is shared silicon with the timer driver (drv/timer.c): the timer
 * driver uses the UPDATE event as a periodic TICK, while this driver uses one
 * of the TIM's compare channels (CC1..CC4) as a PWM output. They cooperate on
 * the SAME TIM_TypeDef:
 *   - the PERIOD (PSC/ARR) and counter start/stop are owned by whichever driver
 *     set them (normally the timer driver in "coordinate" mode);
 *   - this driver only configures the channel (CCMR/CCER) and the duty (CCR),
 *     and claims the output pin through the pinmux (AF mode).
 *
 * Two board-config modes:
 *   freq_hz != 0  -> INDEPENDENT: this driver owns the TIM; it computes PSC/ARR
 *                   for freq_hz and starts/stops the counter itself.
 *   freq_hz == 0  -> COORDINATE : reuse an EXISTING period (the timer driver's).
 *                   This driver must be opened AFTER the timer driver on the same
 *                   TIM, and only touches CCMR/CCER/CCR + the pin. The PWM
 *                   frequency then equals the timer driver's tick rate.
 */
typedef struct _pwm pwm;

/* Board fills this as DATA. timer_clk_hz is the clock feeding the TIM
 * (TIM2..TIM5/12..14 on F4 = 84 MHz APB1; TIM1/8..11 = 168 MHz APB2). */
typedef struct {
    const char *name;        /* logical device name (e.g. "pwm0") */
    void *peripheral;        /* TIM base (board supplies the real silicon) */
    uint32_t timer_clk_hz;   /* clock feeding this TIM */
    uint32_t freq_hz;        /* PWM frequency; 0 = coordinate with existing period */
    int      channel;        /* compare channel 1..4 */
    const char *pin_signal;  /* AF signal name, e.g. "TIM3_CH1_PA6" */
    /* --- advanced-TIM (TIM1/TIM8) options; ignored on a GP TIM --- */
    const char *comp_pin_signal; /* complementary output pin (e.g. "TIM8_CH1N") or NULL */
    uint16_t deadtime_ticks; /* dead-time in timer ticks (0 = none) */
    uint8_t  complementary;  /* bitmask of channels with complementary out (bit ch-1) */
    uint8_t  break_enable;   /* 1 = arm the Break (fault) input */
    uint8_t  break_polarity; /* 0 = break active-low, 1 = break active-high */
} pwm_config_t;

struct _pwm {
    control_device parent;       /* IS-A control_device IS-A device */
    tim_hal_handle_t *hal;       /* opaque HAL handle (may share a TIM with timer) */
    uint32_t timer_clk_hz;       /* cached from config */
    uint32_t freq_hz;            /* cached from config (0 = coordinate) */
    int      channel;            /* 1..4 */
    const char *pin_signal;      /* cached pin signal name */
    pinmux_port_t port;          /* resolved port for pinmux claim + HAL */
    uint8_t  pin;                /* resolved pin */
    uint8_t  af;                 /* resolved AF number */
    /* advanced-TIM cached config (only used when tim_hal_is_advanced) */
    pinmux_port_t comp_port;     /* complementary pin port (or 0) */
    uint8_t  comp_pin;           /* complementary pin */
    uint8_t  comp_af;            /* complementary pin AF */
    uint16_t deadtime_ticks;     /* dead-time in timer ticks */
    uint8_t  complementary;      /* complementary channel bitmask */
    uint8_t  break_enable;       /* 1 = arm break */
    uint8_t  break_polarity;     /* break polarity */
    uint32_t period_ticks;       /* cached ARR + 1 (PWM period) */
    uint32_t duty_ticks;         /* cached CCR (current duty) */
    int      coord;              /* 1 = coordinate mode (freq_hz == 0) */
};

/* uniform create signature (device *(*)(const void *)) for the board node list */
device *pwm_create(const void *config);
void pwm_destroy(pwm *self);

/* ioctl / control commands (driver-specific) */
#define PWM_IOCTL_SET_DUTY_PERCENT  0x20   /* arg = int*  (0..100) */
#define PWM_IOCTL_SET_DUTY_TICKS    0x21   /* arg = uint32_t* (0..period_ticks) */
#define PWM_IOCTL_SET_FREQ          0x22   /* arg = uint32_t* (Hz; independent mode only) */
#define PWM_IOCTL_GET_PERIOD_TICKS  0x23   /* arg = uint32_t* (ARR+1) */
#define PWM_IOCTL_GET_DUTY_TICKS    0x24   /* arg = uint32_t* (current CCR) */
#define PWM_IOCTL_ENABLE_CHANNEL    0x25   /* arg = NULL (CCER.CCxE = 1) */
#define PWM_IOCTL_DISABLE_CHANNEL   0x26   /* arg = NULL (CCER.CCxE = 0) */
#define PWM_IOCTL_GET_BDTR          0x27   /* arg = uint32_t* (raw BDTR; adv TIM) */
#define PWM_IOCTL_GET_COMPLEMENTARY 0x28   /* arg = uint32_t* (bitmask of CCxNE) */

#endif /* PWM_H */
