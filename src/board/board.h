#ifndef BOARD_H
#define BOARD_H

#include "iface/device.h"

/*
 * BOARD RESOURCE DESCRIPTORS — the "device tree" equivalent.
 *
 * All hardware assignment (which peripheral, which pin, baud rate, supply
 * voltage, attached ADC ...) lives here as plain const DATA. No application
 * code ever references ADC1 / USART1 / GPIOD; those symbols exist ONLY in the
 * per-board implementation file (src/board/<board>.c) and inside the HAL.
 *
 * The logical NAME of each device lives in the node (board_dev_t), not in the
 * driver: the driver receives the name and stores it in device.name at init,
 * so the management layer can look devices up by name OR by class.
 */

/* per-device hardware resources (board data) */
typedef struct {
    void *peripheral;     /* e.g. ADC1 (cast from the chip register base) */
    uint32_t channel;     /* default / logical channel */
    uint32_t vdda_mv;     /* supply voltage in mV */
} adc_board_res_t;

typedef struct {
    void *peripheral;     /* e.g. USART1 */
    uint32_t baud;
    uint8_t is_console;   /* 1 => install as the printf console */
} uart_board_res_t;

typedef struct {
    void *peripheral;     /* e.g. GPIOD */
    uint32_t pin;
    uint32_t mode;        /* 0 = in, 1 = out, 2 = alt */
} gpio_board_res_t;

typedef struct {
    int unused;           /* clock needs no hardware resource */
} clock_board_res_t;

typedef struct {
    const char *adc_name;   /* attached ADC device (must be probed first) */
    uint32_t vdda_mv;
} temp_sensor_board_res_t;

/* A board device node: a class (driver_type_t), its logical name, and a pointer
 * to the matching *_board_res_t above. A board descriptor is just an array of
 * these — no switch / no per-type code in the init path. */
typedef struct {
    driver_type_t type;
    const char *name;
    const void *res;
} board_node_t;

typedef struct {
    const board_node_t *nodes;
    uint32_t node_count;
} board_desc_t;

/* Board init: walk the descriptor, build HAL handles + drivers via the per-class
 * probe table (see stm32f4_discovery.c) and register each under its name.
 * Implemented per-board (it is the only code that knows the HAL and the real
 * peripheral bases). */
void board_init(void);

#endif /* BOARD_H */
