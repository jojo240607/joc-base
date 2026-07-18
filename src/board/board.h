#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

/*
 * BOARD RESOURCE DESCRIPTORS — the "device tree" equivalent.
 *
 * All hardware assignment (which peripheral, which pin, baud rate, supply
 * voltage, attached ADC ...) lives here as plain const DATA. No application
 * code ever references ADC1 / USART1 / GPIOD; those symbols exist ONLY in the
 * per-board implementation file (src/board/<board>.c) and inside the HAL.
 *
 * This is the data-ification of what used to be hardcoded in main.c: the board
 * differences become a table, not code.
 */

typedef enum {
    BOARD_DEV_ADC,
    BOARD_DEV_UART,
    BOARD_DEV_GPIO,
    BOARD_DEV_CLOCK,
    BOARD_DEV_TEMP_SENSOR
} board_dev_type_t;

typedef struct {
    const char *name;
    void *peripheral;     /* e.g. ADC1 (cast from the chip register base) */
    uint32_t channel;     /* default / logical channel */
    uint32_t vdda_mv;     /* supply voltage in mV */
} adc_board_res_t;

typedef struct {
    const char *name;
    void *peripheral;     /* e.g. USART1 */
    uint32_t baud;
    uint8_t is_console;   /* 1 => install as the printf console */
} uart_board_res_t;

typedef struct {
    const char *name;
    void *peripheral;     /* e.g. GPIOD */
    uint32_t pin;
    uint32_t mode;        /* 0 = in, 1 = out, 2 = alt */
} gpio_board_res_t;

typedef struct {
    const char *name;
} clock_board_res_t;

typedef struct {
    const char *name;
    const char *adc_name;   /* attached ADC device (must be registered first) */
    uint32_t vdda_mv;
} temp_sensor_board_res_t;

/* Generic node: a type tag + pointer to the matching *_board_res_t above.
 * A board descriptor is just an array of these. */
typedef struct {
    board_dev_type_t type;
    const void *res;
} board_node_t;

typedef struct {
    const board_node_t *nodes;
    uint32_t node_count;
} board_desc_t;

/* Board init: build HAL handles + drivers from the descriptor and register
 * them into the device manager. Implemented per-board (it is the only code
 * that knows the HAL and the concrete peripheral bases). */
void board_init(void);

#endif /* BOARD_H */
