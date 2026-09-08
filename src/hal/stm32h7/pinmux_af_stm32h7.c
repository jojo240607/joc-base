#include "pinmux_hal.h"
#include <string.h>
#include <stdlib.h>      /* atoi — for generic "GPIOx_NN" name parser */

/*
 * STM32H750xx ALTERNATE-FUNCTION TABLE (USART1 + QUADSPI).
 *
 * H7 uses F4-style AF numbers (AFR[2] registers). USART1 = AF7 on PA9/PA10;
 * QUADSPI (bank 1) = AF10 on PA6/PB2/PB6/PB7/PB0/PB1 (STM32H750B-DK W25Q128).
 * The actual pin muxing is done by pinmux_hal_config().
 */

typedef struct {
    pinmux_port_t port;
    uint8_t pin;
    uint8_t af;
    const char *name;
} af_entry_t;

static const af_entry_t g_af[] = {
    /* ---------------- USART1 (PA9/PA10 = AF7) ---------------- */
    {PINMUX_PORT_A, 9,  7, "USART1_TX"},
    {PINMUX_PORT_A, 10, 7, "USART1_RX"},

    /* ---------------- QUADSPI bank 1 (AF10, H750B-DK W25Q128) ---------------- */
    {PINMUX_PORT_A, 6,  10, "QSPI_CLK"},
    {PINMUX_PORT_B, 2,  10, "QSPI_CS"},
    {PINMUX_PORT_B, 6,  10, "QSPI_DQ0"},
    {PINMUX_PORT_B, 7,  10, "QSPI_DQ1"},
    {PINMUX_PORT_B, 0,  10, "QSPI_DQ2"},
    {PINMUX_PORT_B, 1,  10, "QSPI_DQ3"},

    /* ---------------- SDMMC1 (AF12, PC8-12 + PD2, same as F4 SDIO) ---------------- */
    {PINMUX_PORT_C,12, 12, "SDIO_CK"},
    {PINMUX_PORT_D, 2, 12, "SDIO_CMD"},
    {PINMUX_PORT_C, 8, 12, "SDIO_D0"},
    {PINMUX_PORT_C, 9, 12, "SDIO_D1"},
    {PINMUX_PORT_C,10, 12, "SDIO_D2"},
    {PINMUX_PORT_C,11, 12, "SDIO_D3"},
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

    /* Generic GPIO pins: "GPIO<port><pin>" (e.g. "GPIOC_13"). H7 ports A..K. */
    if (signal[0] == 'G' && signal[1] == 'P' && signal[2] == 'I' &&
        signal[3] == 'O' && signal[4] >= 'A' && signal[4] <= 'K' &&
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
