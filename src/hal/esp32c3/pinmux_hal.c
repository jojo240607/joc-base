#include "pinmux_hal.h"
#include <stddef.h>      /* NULL */

/*
 * ESP32-C3 pinmux HAL — Phase-1 stub.
 *
 * No GPIO matrix programming under Renode (no model, registers absorbed) and
 * no AF database (ESP32-C3 routes signals through the GPIO matrix, which is a
 * different mechanism than STM32 AF). All functions are linkable no-ops so the
 * platform-independent pinmux/uart drivers resolve symbols; uart_dev_open()
 * skips the claim step because the board never registers a "pinmux" device.
 */

int pinmux_hal_resolve(const char *signal,
                       pinmux_port_t *port, uint8_t *pin, uint8_t *af)
{
    (void)signal; (void)port; (void)pin; (void)af;
    return 0;       /* no signal database in Phase-1 */
}

const char *pinmux_hal_signal_at(pinmux_port_t port, uint8_t pin, uint8_t af)
{
    (void)port; (void)pin; (void)af;
    return NULL;
}

void pinmux_hal_config(pinmux_port_t port, uint8_t pin,
                       const pinmux_pin_cfg_t *cfg)
{
    (void)port; (void)pin; (void)cfg;
}

void *pinmux_hal_port_base(pinmux_port_t port)
{
    (void)port;
    return NULL;
}
