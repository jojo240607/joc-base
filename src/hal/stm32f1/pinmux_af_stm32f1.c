#include "pinmux_hal.h"
#include <string.h>
#include <stdlib.h>      /* atoi — for generic "GPIOx_NN" name parser */

/*
 * STM32F103xx ALTERNATE-FUNCTION TABLE (minimal: only USART1 for now).
 *
 * F1 does NOT use AF numbers like F4. Alternate functions are selected via
 * CRL/CRH CNF bits (alt func push-pull for TX, floating input for RX, etc.).
 * The af field is always 0 here; the actual pin muxing is handled by
 * pinmux_hal_config().
 */

typedef struct {
    pinmux_port_t port;
    uint8_t pin;
    uint8_t af;          /* unused on F1, kept for API compatibility */
    const char *name;
} af_entry_t;

static const af_entry_t g_af[] = {
    /* ---------------- USART1 (default pins, no remap) ---------------- */
    {PINMUX_PORT_A, 9,  0, "USART1_TX"},
    {PINMUX_PORT_A, 10, 0, "USART1_RX"},

    /* ---------------- USART2 ---------------- */
    {PINMUX_PORT_A, 2,  0, "USART2_TX"},
    {PINMUX_PORT_A, 3,  0, "USART2_RX"},

    /* ---------------- USART3 ---------------- */
    {PINMUX_PORT_B, 10, 0, "USART3_TX"},
    {PINMUX_PORT_B, 11, 0, "USART3_RX"},
};

int pinmux_hal_resolve(const char *signal,
                       pinmux_port_t *port, uint8_t *pin, uint8_t *af)
{
    if (!signal) return 0;
    for (size_t i = 0; i < sizeof(g_af) / sizeof(g_af[0]); i++) {
        if (strcmp(g_af[i].name, signal) == 0) {
            if (port) *port = g_af[i].port;
            if (pin)  *pin  = g_af[i].pin;
            if (af)   *af   = g_af[i].af;
            return 1;
        }
    }

    /* Generic GPIO pins: "GPIO<port><pin>" (e.g. "GPIOC_13") */
    if (signal[0] == 'G' && signal[1] == 'P' && signal[2] == 'I' &&
        signal[3] == 'O' && signal[4] >= 'A' && signal[4] <= 'G' &&
        signal[5] == '_') {
        pinmux_port_t p = (pinmux_port_t)(signal[4] - 'A');
        int n = atoi(&signal[6]);
        if (p < PINMUX_PORT_COUNT && n >= 0 && n < 16) {
            if (port) *port = p;
            if (pin)  *pin  = (uint8_t)n;
            if (af)   *af   = 0;
            return 1;
        }
    }
    return 0;
}

const char *pinmux_hal_signal_at(pinmux_port_t port, uint8_t pin, uint8_t af)
{
    (void)af;
    for (size_t i = 0; i < sizeof(g_af) / sizeof(g_af[0]); i++) {
        if (g_af[i].port == port && g_af[i].pin == pin)
            return g_af[i].name;
    }
    return NULL;
}