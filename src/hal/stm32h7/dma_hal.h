#ifndef DMA_HAL_H
#define DMA_HAL_H

/*
 * Minimal dma_hal.h stub for STM32H750.
 *
 * Milestone 1 skips DMA entirely (all UART I/O uses IRQ mode; DTCM is not
 * DMA-accessible, DMA buffers would need AXI SRAM — deferred to milestone 2).
 * This header provides the types and function declarations that the
 * platform-independent driver layer expects, but with no-op (return NULL/0)
 * implementations — since no H750 board node instantiates DMA, the code
 * paths that call into DMA are never reached.
 */

#include <stdint.h>
#include "irq.h"              /* irq_id_t — the HAL returns the platform irq id */

/* Opaque stream handle */
typedef struct dma_hal_stream dma_hal_stream_t;

dma_hal_stream_t *dma_hal_stream_create(void *dma_periph, uint32_t stream_idx);
void dma_hal_stream_destroy(dma_hal_stream_t *s);
void dma_hal_enable_clock(void *dma_periph);

typedef enum {
    DMA_HAL_DIR_P2M = 0,
    DMA_HAL_DIR_M2P = 1,
    DMA_HAL_DIR_M2M = 2,
} dma_hal_dir_t;

typedef enum {
    DMA_HAL_SIZE_8  = 0,
    DMA_HAL_SIZE_16 = 1,
    DMA_HAL_SIZE_32 = 2,
} dma_hal_size_t;

void dma_hal_stream_config(dma_hal_stream_t *s, dma_hal_dir_t dir, uint32_t channel,
                           const void *periph, void *mem, uint32_t count,
                           dma_hal_size_t size, int periph_inc, int mem_inc,
                           uint32_t prio);
void dma_hal_stream_enable_irq(dma_hal_stream_t *s, int tc, int te);
void dma_hal_stream_disable_irq(dma_hal_stream_t *s);
void dma_hal_stream_set_circular(dma_hal_stream_t *s, int en);
void dma_hal_stream_start(dma_hal_stream_t *s);
void dma_hal_stream_stop(dma_hal_stream_t *s);
int  dma_hal_stream_tc(dma_hal_stream_t *s);
int  dma_hal_stream_te(dma_hal_stream_t *s);
void dma_hal_stream_clear_flags(dma_hal_stream_t *s);
uint32_t dma_hal_stream_remaining(dma_hal_stream_t *s);
irq_id_t dma_hal_stream_irq_id(dma_hal_stream_t *s);
int dma_hal_is_m2m_capable(dma_hal_stream_t *s);

/* Peripheral -> DMA route table — minimal, all requests return empty route */
typedef enum {
    DMA_REQ_NONE = 0,
    DMA_REQ_USART1_TX, DMA_REQ_USART1_RX,
    DMA_REQ_USART2_TX, DMA_REQ_USART2_RX,
    DMA_REQ_USART3_TX, DMA_REQ_USART3_RX,
    DMA_REQ_UART4_TX,  DMA_REQ_UART4_RX,
    DMA_REQ_UART5_TX,  DMA_REQ_UART5_RX,
    DMA_REQ_USART6_TX, DMA_REQ_USART6_RX,
    DMA_REQ_SPI1_TX,  DMA_REQ_SPI1_RX,
    DMA_REQ_SPI2_TX,  DMA_REQ_SPI2_RX,
    DMA_REQ_SPI3_TX,  DMA_REQ_SPI3_RX,
    DMA_REQ_I2C1_TX,  DMA_REQ_I2C1_RX,
    DMA_REQ_I2C2_TX,  DMA_REQ_I2C2_RX,
    DMA_REQ_I2C3_TX,  DMA_REQ_I2C3_RX,
    DMA_REQ_ADC1,
    DMA_REQ_ADC2, DMA_REQ_ADC3,
    DMA_REQ_TIM2_UP, DMA_REQ_TIM3_UP, DMA_REQ_TIM4_UP,
    DMA_REQ_TIM5_UP, DMA_REQ_TIM6_UP, DMA_REQ_TIM7_UP, DMA_REQ_TIM8_UP,
    DMA_REQ_DAC1, DMA_REQ_DAC2,
    DMA_REQ_SDIO,
} dma_req_id_t;

typedef struct {
    const char *name;
    uint8_t     stream;
    uint8_t     channel;
} dma_route_t;

dma_route_t dma_hal_route(dma_req_id_t req);

#endif /* DMA_HAL_H */
