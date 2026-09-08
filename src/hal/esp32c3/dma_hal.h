#ifndef JOC_HAL_ESP32C3_DMA_HAL_H
#define JOC_HAL_ESP32C3_DMA_HAL_H

/*
 * ESP32-C3 DMA HAL stub — Phase-1 Renode.
 *
 * No peripheral DMA is modeled (UART uses IRQ engine, board config sets
 * DMA_REQ_NONE). These types and stubs exist only so the platform-independent
 * driver layer (#include "drv/dma.h") can compile and link.
 */

#include <stdint.h>

/* DMA request ID (mirrors the STM32 enum contract from drv/dma.h).
 * On ESP32-C3 Phase-1 we only need the DMA_REQ_NONE sentinel. */
typedef enum {
    DMA_REQ_NONE = 0,
} dma_req_id_t;

/* DMA route descriptor — returned by dma_hal_route() to tell the driver which
 * DMA controller/stream/channel this peripheral request is wired to.
 * Phase-1: always returns { "none", 0, 0 } — driver does not open DMA. */
typedef struct {
    const char *name;       /* DMA controller device name */
    uint8_t     stream;     /* stream index 0..7 */
    uint8_t     channel;    /* channel (CHSEL) value */
} dma_route_t;

/* Resolve a peripheral DMA request into a (controller, stream, channel) tuple.
 * ESP32-C3 Phase-1: no peripheral DMA, returns a null route. */
dma_route_t dma_hal_route(dma_req_id_t req);

#endif /* JOC_HAL_ESP32C3_DMA_HAL_H */