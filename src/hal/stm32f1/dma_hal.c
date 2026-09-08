/*
 * Minimal dma_hal.c stub for STM32F103.
 *
 * F103 minimal port does not use DMA (all UART I/O uses IRQ mode).
 * This provides the dma_hal_route() stub that the driver layer's
 * uart_dma_acquire path needs for linking.  The code is never reached
 * at runtime because board config uses DMA_REQ_NONE + STREAM_MODE_IRQ.
 */
#include "dma_hal.h"

dma_route_t dma_hal_route(dma_req_id_t req)
{
    (void)req;
    dma_route_t r = { "none", 0xFF, 0xFF };
    return r;
}