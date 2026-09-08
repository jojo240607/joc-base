#include "dma_hal.h"

/*
 * ESP32-C3 DMA HAL stub — Phase-1 Renode.
 *
 * No peripheral DMA is modeled; the board's uart0 config sets DMA_REQ_NONE for
 * both TX and RX, so the driver never calls dma_hal_route() at runtime (it
 * returns 0 early at uart_dma_acquire: "if (dma_tx_req == DMA_REQ_NONE &&
 * dma_rx_req == DMA_REQ_NONE) return 0;"). This stub exists only for the
 * linker to resolve the symbol.
 */

dma_route_t dma_hal_route(dma_req_id_t req)
{
    (void)req;
    dma_route_t r = { "none", 0xFF, 0xFF };
    return r;
}