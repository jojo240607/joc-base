#include "pinmux_hal.h"
#include <string.h>
#include <stdlib.h>      /* atoi — for the generic "GPIOx_NN" name parser */

/*
 * STM32F407xx ALTERNATE-FUNCTION MATRIX
 * =====================================
 * Single source of truth that lets the pinmux resolve a peripheral signal to
 * its (port, pin, AF) and detect when two configurations fight over the same
 * physical pin. Derived from the STM32F407xx datasheet "Alternate function
 * mapping" table and covering every multiplexed peripheral of the chip:
 *
 *   USART1/2/3, UART4/5, USART6      SPI1/2/3, I2S1/2/3
 *   I2C1/2/3                         TIM1..TIM14
 *   CAN1/2                           SDIO
 *   ETH (RMII + MII)                 FSMC/FMC
 *   ADC1/2/3, DAC                    USB OTG FS/HS (ULPI)
 *   DCMI (camera)                    SYS / debug (JTAG-SWD, MCO)
 *
 * Naming: "<PERIPH>_<SIGNAL>" (e.g. "USART1_TX", "SPI2_SCK", "TIM1_CH1",
 * "ADC1_IN0", "ETH_RMII_TXD0"). A signal that can appear on SEVERAL pins gets a
 * "_P<port><pin>" suffix so every name is UNIQUE and the board can pick the
 * exact pad unambiguously — e.g. "USART1_TX_PA9" (PA9) vs "USART1_TX_PB6" (PB6).
 * The pinmux resolves the name the board supplies to its (port, pin, af).
 *
 * Plain GPIO pins are NOT listed here: they are named "GPIO<port><pin>"
 * (e.g. "GPIOD_12") and resolved generically by pinmux_hal_resolve() without a
 * database row, so any GPIO pad is reachable with no per-pin table entry.
 *
 * AF numbers for analog inputs (ADCx_INy, DAC_x) are 0 — the pinmux only uses
 * the (port, pin) for those, which is enough to block conflicting use.
 *
 * NOTE: this is data; if a board uses an unusual remap, extend this table.
 */
typedef struct {
    pinmux_port_t port;
    uint8_t pin;
    uint8_t af;
    const char *name;
} af_entry_t;

static const af_entry_t g_af[] = {
    /* ---------------- USART / UART ---------------- */
    {PINMUX_PORT_A, 9, 7, "USART1_TX_PA9"},
    {PINMUX_PORT_A, 10, 7, "USART1_RX_PA10"},
    {PINMUX_PORT_A, 8, 7, "USART1_CK"},
    {PINMUX_PORT_A,12, 7, "USART1_RTS"},
    {PINMUX_PORT_A,11, 7, "USART1_CTS"},
    {PINMUX_PORT_B, 6, 7, "USART1_TX_PB6"},
    {PINMUX_PORT_B, 7, 7, "USART1_RX_PB7"},

    {PINMUX_PORT_A, 2, 7, "USART2_TX_PA2"},
    {PINMUX_PORT_A, 3, 7, "USART2_RX_PA3"},
    {PINMUX_PORT_A, 4, 7, "USART2_CK_PA4"},
    {PINMUX_PORT_A, 1, 7, "USART2_RTS_PA1"},
    {PINMUX_PORT_A, 0, 7, "USART2_CTS_PA0"},
    {PINMUX_PORT_D, 5, 7, "USART2_TX_PD5"},
    {PINMUX_PORT_D, 6, 7, "USART2_RX_PD6"},
    {PINMUX_PORT_D, 7, 7, "USART2_CK_PD7"},
    {PINMUX_PORT_D, 4, 7, "USART2_RTS_PD4"},
    {PINMUX_PORT_D, 3, 7, "USART2_CTS_PD3"},

    {PINMUX_PORT_B, 10, 7, "USART3_TX_PB10"},
    {PINMUX_PORT_B, 11, 7, "USART3_RX_PB11"},
    {PINMUX_PORT_B, 12, 7, "USART3_CK_PB12"},
    {PINMUX_PORT_B,14, 7, "USART3_RTS"},
    {PINMUX_PORT_B,13, 7, "USART3_CTS"},
    {PINMUX_PORT_C, 10, 7, "USART3_TX_PC10"},
    {PINMUX_PORT_C, 11, 7, "USART3_RX_PC11"},
    {PINMUX_PORT_C, 12, 7, "USART3_CK_PC12"},
    {PINMUX_PORT_D, 8, 7, "USART3_TX_PD8"},
    {PINMUX_PORT_D, 9, 7, "USART3_RX_PD9"},

    {PINMUX_PORT_A, 0, 8, "UART4_TX_PA0"},
    {PINMUX_PORT_A, 1, 8, "UART4_RX_PA1"},
    {PINMUX_PORT_C, 10, 8, "UART4_TX_PC10"},
    {PINMUX_PORT_C, 11, 8, "UART4_RX_PC11"},

    {PINMUX_PORT_C,12, 8, "UART5_TX"},
    {PINMUX_PORT_D, 2, 8, "UART5_RX"},

    {PINMUX_PORT_C, 6, 8, "USART6_TX"},
    {PINMUX_PORT_C, 7, 8, "USART6_RX"},
    {PINMUX_PORT_C, 8, 8, "USART6_CK"},
    {PINMUX_PORT_G, 8, 8, "USART6_RTS"},
    {PINMUX_PORT_G, 9, 8, "USART6_CTS"},

    /* ---------------- SPI / I2S ---------------- */
    {PINMUX_PORT_A, 5, 5, "SPI1_SCK_PA5"},
    {PINMUX_PORT_A, 6, 5, "SPI1_MISO_PA6"},
    {PINMUX_PORT_A, 7, 5, "SPI1_MOSI_PA7"},
    {PINMUX_PORT_A, 4, 5, "SPI1_NSS_PA4"},
    {PINMUX_PORT_B, 3, 5, "SPI1_SCK_PB3"},
    {PINMUX_PORT_B, 4, 5, "SPI1_MISO_PB4"},
    {PINMUX_PORT_B, 5, 5, "SPI1_MOSI_PB5"},
    {PINMUX_PORT_A, 15, 5, "SPI1_NSS_PA15"},

    {PINMUX_PORT_B, 13, 5, "SPI2_SCK_PB13"},
    {PINMUX_PORT_B, 14, 5, "SPI2_MISO_PB14"},
    {PINMUX_PORT_B, 15, 5, "SPI2_MOSI_PB15"},
    {PINMUX_PORT_B, 12, 5, "SPI2_NSS_PB12"},
    {PINMUX_PORT_I, 1, 5, "SPI2_SCK_PI1"},
    {PINMUX_PORT_I, 2, 5, "SPI2_MISO_PI2"},
    {PINMUX_PORT_I, 3, 5, "SPI2_MOSI_PI3"},
    {PINMUX_PORT_I, 0, 5, "SPI2_NSS_PI0"},
    {PINMUX_PORT_C, 1, 5, "I2S2_EXTSD"},

    {PINMUX_PORT_B, 3, 6, "SPI3_SCK_PB3"},
    {PINMUX_PORT_B, 4, 6, "SPI3_MISO_PB4"},
    {PINMUX_PORT_B, 5, 6, "SPI3_MOSI_PB5"},
    {PINMUX_PORT_A,15, 6, "SPI3_NSS"},
    {PINMUX_PORT_C, 10, 6, "SPI3_SCK_PC10"},
    {PINMUX_PORT_C, 11, 6, "SPI3_MISO_PC11"},
    {PINMUX_PORT_C, 12, 6, "SPI3_MOSI_PC12"},

    /* I2S1 shares SPI1 pins on AF5 */
    {PINMUX_PORT_A, 4, 5, "I2S1_WS"},
    {PINMUX_PORT_A, 5, 5, "I2S1_CK"},
    {PINMUX_PORT_A, 7, 5, "I2S1_SD"},
    {PINMUX_PORT_C, 4, 5, "I2S1_MCK"},

    /* ---------------- I2C ---------------- */
    {PINMUX_PORT_B, 6, 4, "I2C1_SCL_PB6"},
    {PINMUX_PORT_B, 7, 4, "I2C1_SDA_PB7"},
    {PINMUX_PORT_B, 5, 4, "I2C1_SMBA"},
    {PINMUX_PORT_B, 8, 4, "I2C1_SCL_PB8"},
    {PINMUX_PORT_B, 9, 4, "I2C1_SDA_PB9"},

    {PINMUX_PORT_B,10, 4, "I2C2_SCL_PB10"},
    {PINMUX_PORT_B,10, 4, "I2C2_SCL"},
    {PINMUX_PORT_B,11, 4, "I2C2_SDA_PB11"},
    {PINMUX_PORT_B,11, 4, "I2C2_SDA"},
    {PINMUX_PORT_B,12, 4, "I2C2_SMBA"},

    {PINMUX_PORT_A, 8, 4, "I2C3_SCL_PA8"},
    {PINMUX_PORT_A, 8, 4, "I2C3_SCL"},
    {PINMUX_PORT_C, 9, 4, "I2C3_SDA_PC9"},
    {PINMUX_PORT_C, 9, 4, "I2C3_SDA"},
    {PINMUX_PORT_A, 9, 4, "I2C3_SMBA"},

    /* ---------------- TIM1 (AF1) ---------------- */
    {PINMUX_PORT_A, 8, 1, "TIM1_CH1_PA8"},
    {PINMUX_PORT_A, 9, 1, "TIM1_CH2_PA9"},
    {PINMUX_PORT_A, 10, 1, "TIM1_CH3_PA10"},
    {PINMUX_PORT_A, 11, 1, "TIM1_CH4_PA11"},
    {PINMUX_PORT_A, 7, 1, "TIM1_CH1N_PA7"},
    {PINMUX_PORT_A, 6, 1, "TIM1_BKIN_PA6"},
    {PINMUX_PORT_A, 12, 1, "TIM1_ETR_PA12"},
    {PINMUX_PORT_E, 9, 1, "TIM1_CH1_PE9"},
    {PINMUX_PORT_E, 11, 1, "TIM1_CH2_PE11"},
    {PINMUX_PORT_E, 13, 1, "TIM1_CH3_PE13"},
    {PINMUX_PORT_E, 14, 1, "TIM1_CH4_PE14"},
    {PINMUX_PORT_E, 8, 1, "TIM1_CH1N_PE8"},
    {PINMUX_PORT_E,10, 1, "TIM1_CH2N"},
    {PINMUX_PORT_E,12, 1, "TIM1_CH3N"},
    {PINMUX_PORT_E, 15, 1, "TIM1_BKIN_PE15"},
    {PINMUX_PORT_E, 7, 1, "TIM1_ETR_PE7"},

    /* ---------------- TIM2 (AF1) ---------------- */
    {PINMUX_PORT_A, 0, 1, "TIM2_CH1_PA0"},
    {PINMUX_PORT_A, 1, 1, "TIM2_CH2_PA1"},
    {PINMUX_PORT_A, 2, 1, "TIM2_CH3_PA2"},
    {PINMUX_PORT_A, 3, 1, "TIM2_CH4_PA3"},
    {PINMUX_PORT_A, 15, 1, "TIM2_CH1_PA15"},
    {PINMUX_PORT_B, 3, 1, "TIM2_CH2_PB3"},
    {PINMUX_PORT_B, 10, 1, "TIM2_CH3_PB10"},
    {PINMUX_PORT_B, 11, 1, "TIM2_CH4_PB11"},

    /* ---------------- TIM3 (AF2) ---------------- */
    {PINMUX_PORT_A, 6, 2, "TIM3_CH1_PA6"},
    {PINMUX_PORT_A, 7, 2, "TIM3_CH2_PA7"},
    {PINMUX_PORT_B, 0, 2, "TIM3_CH3_PB0"},
    {PINMUX_PORT_B, 1, 2, "TIM3_CH4_PB1"},
    {PINMUX_PORT_C, 6, 2, "TIM3_CH1_PC6"},
    {PINMUX_PORT_C, 7, 2, "TIM3_CH2_PC7"},
    {PINMUX_PORT_C, 8, 2, "TIM3_CH3_PC8"},
    {PINMUX_PORT_C, 9, 2, "TIM3_CH4_PC9"},
    {PINMUX_PORT_E, 2, 2, "TIM3_CH1_PE2"},
    {PINMUX_PORT_E, 3, 2, "TIM3_CH2_PE3"},
    {PINMUX_PORT_E, 4, 2, "TIM3_CH3_PE4"},
    {PINMUX_PORT_E, 5, 2, "TIM3_CH4_PE5"},

    /* ---------------- TIM4 (AF2) ---------------- */
    {PINMUX_PORT_B, 6, 2, "TIM4_CH1_PB6"},
    {PINMUX_PORT_B, 7, 2, "TIM4_CH2_PB7"},
    {PINMUX_PORT_B, 8, 2, "TIM4_CH3_PB8"},
    {PINMUX_PORT_B, 9, 2, "TIM4_CH4_PB9"},
    {PINMUX_PORT_D, 12, 2, "TIM4_CH1_PD12"},
    {PINMUX_PORT_D, 13, 2, "TIM4_CH2_PD13"},
    {PINMUX_PORT_D, 14, 2, "TIM4_CH3_PD14"},
    {PINMUX_PORT_D, 15, 2, "TIM4_CH4_PD15"},
    {PINMUX_PORT_E, 0, 2, "TIM4_CH1_PE0"},
    {PINMUX_PORT_E, 1, 2, "TIM4_CH2_PE1"},

    /* ---------------- TIM5 (AF2) ---------------- */
    {PINMUX_PORT_A, 0, 2, "TIM5_CH1_PA0"},
    {PINMUX_PORT_A, 1, 2, "TIM5_CH2_PA1"},
    {PINMUX_PORT_A, 2, 2, "TIM5_CH3_PA2"},
    {PINMUX_PORT_A, 3, 2, "TIM5_CH4_PA3"},
    {PINMUX_PORT_H, 10, 2, "TIM5_CH1_PH10"},
    {PINMUX_PORT_H, 11, 2, "TIM5_CH2_PH11"},
    {PINMUX_PORT_H, 12, 2, "TIM5_CH3_PH12"},
    {PINMUX_PORT_I, 0, 2, "TIM5_CH4_PI0"},

    /* ---------------- TIM8 (AF3) ---------------- */
    {PINMUX_PORT_C, 6, 3, "TIM8_CH1_PC6"},
    {PINMUX_PORT_C, 7, 3, "TIM8_CH2_PC7"},
    {PINMUX_PORT_C, 8, 3, "TIM8_CH3_PC8"},
    {PINMUX_PORT_C, 9, 3, "TIM8_CH4_PC9"},
    {PINMUX_PORT_A, 7, 3, "TIM8_CH1N"},
    {PINMUX_PORT_A, 6, 3, "TIM8_BKIN"},
    {PINMUX_PORT_A, 0, 3, "TIM8_ETR"},
    {PINMUX_PORT_I, 5, 3, "TIM8_CH1_PI5"},
    {PINMUX_PORT_I, 6, 3, "TIM8_CH2_PI6"},
    {PINMUX_PORT_I, 7, 3, "TIM8_CH3_PI7"},
    {PINMUX_PORT_I, 2, 3, "TIM8_CH4_PI2"},

    /* ---------------- TIM9 (AF3) ---------------- */
    {PINMUX_PORT_A, 2, 3, "TIM9_CH1_PA2"},
    {PINMUX_PORT_A, 3, 3, "TIM9_CH2_PA3"},
    {PINMUX_PORT_E, 5, 3, "TIM9_CH1_PE5"},
    {PINMUX_PORT_E, 6, 3, "TIM9_CH2_PE6"},

    /* ---------------- TIM10 (AF3) ---------------- */
    {PINMUX_PORT_B, 8, 3, "TIM10_CH1_PB8"},
    {PINMUX_PORT_F, 6, 3, "TIM10_CH1_PF6"},

    /* ---------------- TIM11 (AF3) ---------------- */
    {PINMUX_PORT_B, 9, 3, "TIM11_CH1_PB9"},
    {PINMUX_PORT_F, 7, 3, "TIM11_CH1_PF7"},

    /* ---------------- TIM12 (AF9) ---------------- */
    {PINMUX_PORT_B, 14, 9, "TIM12_CH1_PB14"},
    {PINMUX_PORT_B, 15, 9, "TIM12_CH2_PB15"},
    {PINMUX_PORT_C, 4, 9, "TIM12_CH1_PC4"},
    {PINMUX_PORT_C, 5, 9, "TIM12_CH2_PC5"},

    /* ---------------- TIM13 (AF9) ---------------- */
    {PINMUX_PORT_A, 6, 9, "TIM13_CH1_PA6"},
    {PINMUX_PORT_F, 8, 9, "TIM13_CH1_PF8"},

    /* ---------------- TIM14 (AF9) ---------------- */
    {PINMUX_PORT_A, 7, 9, "TIM14_CH1_PA7"},
    {PINMUX_PORT_F, 9, 9, "TIM14_CH1_PF9"},

    /* ---------------- CAN ---------------- */
    {PINMUX_PORT_A, 12, 9, "CAN1_TX_PA12"},
    {PINMUX_PORT_A, 11, 9, "CAN1_RX_PA11"},
    {PINMUX_PORT_B, 9, 9, "CAN1_TX_PB9"},
    {PINMUX_PORT_B, 8, 9, "CAN1_RX_PB8"},
    {PINMUX_PORT_D, 1, 9, "CAN1_TX_PD1"},
    {PINMUX_PORT_D, 0, 9, "CAN1_RX_PD0"},

    {PINMUX_PORT_B,13, 9, "CAN2_TX"},
    {PINMUX_PORT_B,12, 9, "CAN2_RX"},

    /* ---------------- SDIO (AF12) ---------------- */
    {PINMUX_PORT_C,12,12, "SDIO_CK"},
    {PINMUX_PORT_D, 2,12, "SDIO_CMD"},
    {PINMUX_PORT_C, 8,12, "SDIO_D0"},
    {PINMUX_PORT_C, 9,12, "SDIO_D1"},
    {PINMUX_PORT_C,10,12, "SDIO_D2"},
    {PINMUX_PORT_C,11,12, "SDIO_D3"},
    {PINMUX_PORT_B, 8,12, "SDIO_D4"},
    {PINMUX_PORT_B, 9,12, "SDIO_D5"},
    {PINMUX_PORT_C, 6,12, "SDIO_D6"},
    {PINMUX_PORT_C, 7,12, "SDIO_D7"},

    /* ---------------- ETH RMII / MII (AF11) ---------------- */
    {PINMUX_PORT_A, 1,11, "ETH_REF_CLK"},
    {PINMUX_PORT_A, 2,11, "ETH_MDIO"},
    {PINMUX_PORT_C, 1,11, "ETH_MDC"},
    {PINMUX_PORT_A, 7,11, "ETH_CRS_DV"},
    {PINMUX_PORT_C, 4,11, "ETH_RXD0"},
    {PINMUX_PORT_C, 5,11, "ETH_RXD1"},
    {PINMUX_PORT_B,12,11, "ETH_TXD0"},
    {PINMUX_PORT_B,13,11, "ETH_TXD1"},
    {PINMUX_PORT_B,11,11, "ETH_TX_EN"},
    {PINMUX_PORT_A, 0,11, "ETH_CRS"},    /* MII */
    {PINMUX_PORT_A, 3,11, "ETH_COL"},    /* MII */
    {PINMUX_PORT_B, 0,11, "ETH_RXD2"},   /* MII */
    {PINMUX_PORT_B, 1,11, "ETH_RXD3"},   /* MII */
    {PINMUX_PORT_B,10,11, "ETH_RX_ER"},  /* MII */
    {PINMUX_PORT_C, 2,11, "ETH_TXD2"},   /* MII */
    {PINMUX_PORT_C, 3,11, "ETH_TXD3"},   /* MII */
    {PINMUX_PORT_C, 3,11, "ETH_TX_CLK"}, /* MII */

    /* ---------------- FSMC / FMC (AF12) ---------------- */
    /* control */
    {PINMUX_PORT_D, 4,12, "FSMC_NOE"},
    {PINMUX_PORT_D, 5,12, "FSMC_NWE"},
    {PINMUX_PORT_D, 7,12, "FSMC_NE1"},
    {PINMUX_PORT_G, 9,12, "FSMC_NE2"},
    {PINMUX_PORT_G,10,12, "FSMC_NE3"},
    {PINMUX_PORT_G,12,12, "FSMC_NE4"},
    {PINMUX_PORT_D, 6,12, "FSMC_NWAIT"},
    {PINMUX_PORT_D, 6,12, "FSMC_NADV"},
    /* data bus D0..D15 */
    {PINMUX_PORT_D,14,12, "FSMC_D0"},
    {PINMUX_PORT_D,15,12, "FSMC_D1"},
    {PINMUX_PORT_D, 0,12, "FSMC_D2"},
    {PINMUX_PORT_D, 1,12, "FSMC_D3"},
    {PINMUX_PORT_E, 7,12, "FSMC_D4"},
    {PINMUX_PORT_E, 8,12, "FSMC_D5"},
    {PINMUX_PORT_E, 9,12, "FSMC_D6"},
    {PINMUX_PORT_E,10,12, "FSMC_D7"},
    {PINMUX_PORT_E,11,12, "FSMC_D8"},
    {PINMUX_PORT_E,12,12, "FSMC_D9"},
    {PINMUX_PORT_E,13,12, "FSMC_D10"},
    {PINMUX_PORT_E,14,12, "FSMC_D11"},
    {PINMUX_PORT_E,15,12, "FSMC_D12"},
    {PINMUX_PORT_D, 8,12, "FSMC_D13"},
    {PINMUX_PORT_D, 9,12, "FSMC_D14"},
    {PINMUX_PORT_D,10,12, "FSMC_D15"},
    /* address bus A0..A18 (commonly used subset) */
    {PINMUX_PORT_F, 0,12, "FSMC_A0"},
    {PINMUX_PORT_F, 1,12, "FSMC_A1"},
    {PINMUX_PORT_F, 2,12, "FSMC_A2"},
    {PINMUX_PORT_F, 3,12, "FSMC_A3"},
    {PINMUX_PORT_F, 4,12, "FSMC_A4"},
    {PINMUX_PORT_F, 5,12, "FSMC_A5"},
    {PINMUX_PORT_F,12,12, "FSMC_A6"},
    {PINMUX_PORT_F,13,12, "FSMC_A7"},
    {PINMUX_PORT_F,14,12, "FSMC_A8"},
    {PINMUX_PORT_F,15,12, "FSMC_A9"},
    {PINMUX_PORT_G, 0,12, "FSMC_A10"},
    {PINMUX_PORT_G, 1,12, "FSMC_A11"},
    {PINMUX_PORT_G, 2,12, "FSMC_A12"},
    {PINMUX_PORT_G, 3,12, "FSMC_A13"},
    {PINMUX_PORT_G, 4,12, "FSMC_A14"},
    {PINMUX_PORT_G, 5,12, "FSMC_A15"},
    {PINMUX_PORT_D,11,12, "FSMC_A16"},
    {PINMUX_PORT_D,12,12, "FSMC_A17"},
    {PINMUX_PORT_D,13,12, "FSMC_A18"},

    /* ---------------- ADC / DAC (analog: af = 0) ---------------- */
    {PINMUX_PORT_A, 0, 0, "ADC1_IN0"},
    {PINMUX_PORT_A, 1, 0, "ADC1_IN1"},
    {PINMUX_PORT_A, 2, 0, "ADC1_IN2"},
    {PINMUX_PORT_A, 3, 0, "ADC1_IN3"},
    {PINMUX_PORT_A, 4, 0, "ADC1_IN4"},
    {PINMUX_PORT_A, 5, 0, "ADC1_IN5"},
    {PINMUX_PORT_A, 6, 0, "ADC1_IN6"},
    {PINMUX_PORT_A, 7, 0, "ADC1_IN7"},
    {PINMUX_PORT_B, 0, 0, "ADC1_IN8"},
    {PINMUX_PORT_B, 1, 0, "ADC1_IN9"},
    {PINMUX_PORT_C, 0, 0, "ADC1_IN10"},
    {PINMUX_PORT_C, 1, 0, "ADC1_IN11"},
    {PINMUX_PORT_C, 2, 0, "ADC1_IN12"},
    {PINMUX_PORT_C, 3, 0, "ADC1_IN13"},
    {PINMUX_PORT_C, 4, 0, "ADC1_IN14"},
    {PINMUX_PORT_C, 5, 0, "ADC1_IN15"},
    {PINMUX_PORT_F, 6, 0, "ADC3_IN4"},
    {PINMUX_PORT_F, 7, 0, "ADC3_IN5"},
    {PINMUX_PORT_F, 8, 0, "ADC3_IN6"},
    {PINMUX_PORT_F, 9, 0, "ADC3_IN7"},
    {PINMUX_PORT_F,10, 0, "ADC3_IN8"},
    {PINMUX_PORT_F, 3, 0, "ADC3_IN9"},
    {PINMUX_PORT_F, 4, 0, "ADC3_IN10"},
    {PINMUX_PORT_F, 5, 0, "ADC3_IN11"},
    {PINMUX_PORT_A, 4, 0, "DAC_OUT1"},
    {PINMUX_PORT_A, 5, 0, "DAC_OUT2"},

    /* ---------------- USB OTG ---------------- */
    {PINMUX_PORT_A,11,10, "USB_OTG_FS_DM"},
    {PINMUX_PORT_A,12,10, "USB_OTG_FS_DP"},
    {PINMUX_PORT_A,10,10, "USB_OTG_FS_ID"},
    {PINMUX_PORT_A, 8,10, "USB_OTG_FS_SOF"},
    {PINMUX_PORT_A, 9,10, "USB_OTG_FS_VBUS"},
    {PINMUX_PORT_B,14,12, "USB_OTG_HS_DM"},
    {PINMUX_PORT_B,15,12, "USB_OTG_HS_DP"},
    {PINMUX_PORT_A, 3,10, "USB_OTG_HS_ULPI_D0"},
    {PINMUX_PORT_B, 0,10, "USB_OTG_HS_ULPI_D1"},
    {PINMUX_PORT_B, 1,10, "USB_OTG_HS_ULPI_D2"},
    {PINMUX_PORT_B,10,10, "USB_OTG_HS_ULPI_D3"},
    {PINMUX_PORT_B,11,10, "USB_OTG_HS_ULPI_D4"},
    {PINMUX_PORT_B,12,10, "USB_OTG_HS_ULPI_D5"},
    {PINMUX_PORT_B,13,10, "USB_OTG_HS_ULPI_D6"},
    {PINMUX_PORT_B, 5,10, "USB_OTG_HS_ULPI_D7"},
    {PINMUX_PORT_A, 5,10, "USB_OTG_HS_ULPI_CK"},
    {PINMUX_PORT_C, 2,10, "USB_OTG_HS_ULPI_DIR"},
    {PINMUX_PORT_C, 0,10, "USB_OTG_HS_ULPI_STP"},
    {PINMUX_PORT_C, 3,10, "USB_OTG_HS_ULPI_NXT"},

    /* ---------------- DCMI (camera, AF13) ---------------- */
    {PINMUX_PORT_A, 6,13, "DCMI_PIXCK"},
    {PINMUX_PORT_A, 4,13, "DCMI_HSYNC"},
    {PINMUX_PORT_A, 5,13, "DCMI_VSYNC"},
    {PINMUX_PORT_C, 6,13, "DCMI_D0"},
    {PINMUX_PORT_C, 7,13, "DCMI_D1"},
    {PINMUX_PORT_C, 8,13, "DCMI_D2"},
    {PINMUX_PORT_C, 9,13, "DCMI_D3"},
    {PINMUX_PORT_C,10,13, "DCMI_D4"},
    {PINMUX_PORT_C,11,13, "DCMI_D5"},
    {PINMUX_PORT_C,12,13, "DCMI_D6"},
    {PINMUX_PORT_D, 2,13, "DCMI_D7"},

    /* ---------------- system / debug (AF0) ---------------- */
    {PINMUX_PORT_A,13, 0, "SYS_JTMS_SWDIO"},
    {PINMUX_PORT_A,14, 0, "SYS_JTCK_SWCLK"},
    {PINMUX_PORT_A,15, 0, "SYS_JTDI"},
    {PINMUX_PORT_B, 3, 0, "SYS_JTDO_TRACESWO"},
    {PINMUX_PORT_B, 4, 0, "SYS_JNTRST"},
    {PINMUX_PORT_A, 8, 0, "SYS_MCO1"},
    {PINMUX_PORT_C, 9, 0, "SYS_MCO2"},
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

    /* Generic GPIO pins: "GPIO<port><pin>" (e.g. "GPIOD_12") resolve without a
     * database row, so any GPIO pad is reachable by name. af is 0 (GPIO). */
    if (signal[0] == 'G' && signal[1] == 'P' && signal[2] == 'I' &&
        signal[3] == 'O' && signal[4] >= 'A' && signal[4] <= 'I' &&
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
    for (size_t i = 0; i < sizeof(g_af) / sizeof(g_af[0]); i++) {
        if (g_af[i].port == port && g_af[i].pin == pin && g_af[i].af == af)
            return g_af[i].name;
    }
    return NULL;
}
