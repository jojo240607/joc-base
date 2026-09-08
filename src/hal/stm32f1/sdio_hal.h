#ifndef SDIO_HAL_H
#define SDIO_HAL_H

#include <stdint.h>
#include "irq.h"

/* Minimal stub for F103. SDIO not used on this port. */
typedef struct sdio_hal_handle sdio_hal_handle_t;

sdio_hal_handle_t *sdio_hal_create(void *peripheral);
void sdio_hal_destroy(sdio_hal_handle_t *h);
void sdio_hal_init(sdio_hal_handle_t *h);
void sdio_hal_deinit(sdio_hal_handle_t *h);
int  sdio_hal_card_detect(sdio_hal_handle_t *h);
int  sdio_hal_transfer(sdio_hal_handle_t *h, uint32_t *buf, uint32_t sector, uint32_t count, int write);

#endif /* SDIO_HAL_H */