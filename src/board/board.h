#ifndef BOARD_H
#define BOARD_H

#include "iface/device.h"

/*
 * BOARD NODE — a (create-fn, config) pair.
 *
 * Each driver defines its OWN config struct (in its drv/ header) and a create
 * function with the UNIFORM signature  device *(*)(const void *config)  — the
 * same signature as the board node. The board layer only instantiates those
 * configs as DATA and lists each driver's create fn + its config as a node; the
 * generic board_build() forwards the config pointer to the node's create fn and
 * registers the resulting device by the name the driver set.
 *
 * Because dispatch is "config + its own create fn", there is NO switch on device
 * type anywhere and NO per-driver build wrapper. Adding a driver class means:
 * add its config struct + create fn in drv/ and one node in the board array —
 * nothing else changes.
 */

/* uniform create signature — every driver's create fn matches this */
typedef device *(*driver_create_t)(const void *config);

typedef struct {
    driver_create_t create;   /* driver-provided create fn (uniform signature) */
    const void *config;       /* driver-provided config (driver-specific type) */
} board_node_t;

/* Board init: walk the board's node array, build each device from its config
 * via the node's create fn, and register it under the name the driver set.
 * Implemented per-board (it is the only code that knows the HAL and the real
 * peripheral bases). */
void board_init(void);

/* Board-level SysTick tick service (demonstrates the platform-independent irq
 * framework for a core exception). Starts a 1 kHz tick and registers its ISR
 * through irq_register(); board_ticks() returns the elapsed tick count. */
void board_tick_init(void);
uint32_t board_ticks(void);

#endif /* BOARD_H */
