#ifndef DMA_HAL_H
#define DMA_HAL_H

#include <stdint.h>
#include "irq.h"              /* irq_id_t — the HAL returns the platform irq id */

/*
 * Hardware Abstraction Layer — DMA (this file is the STM32F4 implementation).
 *
 * The driver layer (drv/dma.c) includes THIS header but NEVER sees DMA_TypeDef
 * or any other chip register: it only ever handles the OPAQUE
 * `dma_hal_stream_t *`. All register knowledge lives in dma_hal.c. To port to
 * another chip you rewrite this HAL (and its .c) only — the driver source is
 * untouched.
 *
 * STM32F4 DMA model: two controllers (DMA1/DMA2), each with 8 independent
 * streams. A stream is the unit of a transfer; the CHSEL field selects which
 * peripheral REQUEST is routed to that stream. Memory-to-memory transfers need
 * no peripheral and start as soon as EN is set. Per-stream TC/HT/TE/... flags
 * live in the controller's LISR/HISR (and are cleared via LIFCR/HIFCR); each
 * stream has its OWN dedicated IRQ line (DMAx_Streamy_IRQn).
 */
typedef struct dma_hal_stream dma_hal_stream_t;

/* platform-specific construction: the board passes the real controller base
 * (DMA1/DMA2, cast to void*) and the stream index (0..7). Everything else is
 * hidden inside the handle. */
dma_hal_stream_t *dma_hal_stream_create(void *dma_periph, uint32_t stream_idx);
void dma_hal_stream_destroy(dma_hal_stream_t *s);

/* Enable the controller's AHB clock (RCC AHB1ENR DMA1EN / DMA2EN). */
void dma_hal_enable_clock(void *dma_periph);

/* Transfer direction (matches the driver-layer dma_dir_t integer values, and
 * the hardware DIR[7:6] bits: 00=P2M, 01=M2P, 10=M2M). */
typedef enum {
    DMA_HAL_DIR_P2M = 0,   /* peripheral -> memory (DIR=00) */
    DMA_HAL_DIR_M2P = 1,   /* memory -> peripheral (DIR=01) */
    DMA_HAL_DIR_M2M = 2,   /* memory -> memory (DIR=10) */
} dma_hal_dir_t;

/* Data unit size (matches the driver-layer dma_data_size_t integer values). */
typedef enum {
    DMA_HAL_SIZE_8  = 0,
    DMA_HAL_SIZE_16 = 1,
    DMA_HAL_SIZE_32 = 2,
} dma_hal_size_t;

/* Program a stream. `periph` is the address loaded into PAR (the "peripheral"
 * side — for M2M this is just the second memory address); `mem` goes into M0AR.
 * `periph_inc` / `mem_inc` control PINC / MINC. `prio` is the PL field (0..3).
 * The stream is left DISABLED (EN=0); dma_hal_stream_start() arms it.
 * NOTE: CR may only be written while EN=0, so this clears EN first and waits. */
void dma_hal_stream_config(dma_hal_stream_t *s, dma_hal_dir_t dir, uint32_t channel,
                           const void *periph, void *mem, uint32_t count,
                           dma_hal_size_t size, int periph_inc, int mem_inc,
                           uint32_t prio);

/* Enable / disable the Transfer-Complete and Transfer-Error interrupts (TCIE /
 * TEIE). Must be called while EN=0 (i.e. before start). */
void dma_hal_stream_enable_irq(dma_hal_stream_t *s, int tc, int te);
void dma_hal_stream_disable_irq(dma_hal_stream_t *s);

/* Enable / disable CIRCULAR mode (CIRC bit). In circular mode the stream's
 * NDTR wraps to its initial value when it underflows, so the transfer runs
 * FOREVER without a Transfer-Complete — this is exactly what a UART idle-line
 * receiver needs (keep draining DR into a ring; the IDLE ISR reads NDTR to see
 * how much arrived). CIRC may only be written while EN=0, so this clears EN and
 * waits first. */
void dma_hal_stream_set_circular(dma_hal_stream_t *s, int en);

/* Arm the stream (set EN=1). For M2M the transfer starts immediately. */
void dma_hal_stream_start(dma_hal_stream_t *s);
/* Disarm (clear EN=1). */
void dma_hal_stream_stop(dma_hal_stream_t *s);

/* Transfer-complete flag (TCIF) for this stream. */
int  dma_hal_stream_tc(dma_hal_stream_t *s);
/* Transfer-error flag (TEIF) for this stream. */
int  dma_hal_stream_te(dma_hal_stream_t *s);
/* Clear ALL interrupt flags (FE/DME/TE/HT/TC) for this stream. */
void dma_hal_stream_clear_flags(dma_hal_stream_t *s);
/* Remaining transfer count (NDTR). */
uint32_t dma_hal_stream_remaining(dma_hal_stream_t *s);

/* Chip IRQn for this stream (DMA1_Stream0_IRQn .. DMA2_Stream7_IRQn), so the
 * driver can register its ISR through the platform-independent irq framework. */
irq_id_t dma_hal_stream_irq_id(dma_hal_stream_t *s);

/* STM32F4 hardware quirk: memory-to-memory transfers are supported ONLY on
 * DMA2. Requesting M2M on a DMA1 stream is silently a no-op (EN sets but NDTR
 * never decrements, TC never fires) — the driver uses this to reject such
 * requests up front instead of hanging. Returns 1 if this stream's controller
 * can do M2M (i.e. DMA2), 0 otherwise (DMA1). */
int dma_hal_is_m2m_capable(dma_hal_stream_t *s);

/* ---------------------------------------------------------------------------
 * Peripheral -> DMA route table (STM32F4, RM0090 Table 30/31).
 *
 * Unlike memory-to-memory (any free stream works), a PERIPHERAL request is
 * hard-wired by the silicon to ONE specific (controller, stream, channel)
 * triple. The driver names the logical request (e.g. DMA_REQ_USART1_TX) and
 * this table returns the exact stream it must acquire plus the channel to
 * program into CHSEL — there is no freedom to pick another stream. The
 * controller is returned as a device-manager NAME ("dma1"/"dma2") so the
 * driver can resolve it with device_manager_get() without ever touching a
 * DMA_TypeDef. These IDs are the only thing the (platform-independent) drivers
 * need to know; the mapping itself stays chip-specific, HERE in the HAL.
 * ------------------------------------------------------------------------- */
typedef enum {
    DMA_REQ_NONE = 0,
    /* USART / UART — all on DMA channel 4 */
    DMA_REQ_USART1_TX, DMA_REQ_USART1_RX,
    DMA_REQ_USART2_TX, DMA_REQ_USART2_RX,
    DMA_REQ_USART3_TX, DMA_REQ_USART3_RX,
    DMA_REQ_UART4_TX,  DMA_REQ_UART4_RX,
    DMA_REQ_UART5_TX,  DMA_REQ_UART5_RX,
    /* SPI / I2S (I2S2 = SPI2, I2S3 = SPI3) — all on DMA channel 3 */
    DMA_REQ_SPI1_TX,  DMA_REQ_SPI1_RX,
    DMA_REQ_SPI2_TX,  DMA_REQ_SPI2_RX,   /* == I2S2 TX / RX */
    DMA_REQ_SPI3_TX,  DMA_REQ_SPI3_RX,   /* == I2S3 TX / RX */
    /* I2C — DMA1 only */
    DMA_REQ_I2C1_TX,  DMA_REQ_I2C1_RX,
    DMA_REQ_I2C2_TX,  DMA_REQ_I2C2_RX,
    DMA_REQ_I2C3_TX,  DMA_REQ_I2C3_RX,
    /* ADC — DMA2, channel 0 */
    DMA_REQ_ADC1, DMA_REQ_ADC2, DMA_REQ_ADC3,
    /* DAC — DMA1, channel 7 */
    DMA_REQ_DAC1, DMA_REQ_DAC2,
    /* SDIO — DMA2, channel 4 */
    DMA_REQ_SDIO,
    /* TIMER update-event DMA requests (TIMx_UP). A TIM's overflow (Update) event
     * is itself a DMA request source — a stream can move a word into a CCR (e.g.
     * a CPU-less PWM duty sweep) on every overflow. */
    DMA_REQ_TIM2_UP, DMA_REQ_TIM3_UP, DMA_REQ_TIM4_UP,
    DMA_REQ_TIM5_UP, DMA_REQ_TIM6_UP, DMA_REQ_TIM7_UP, DMA_REQ_TIM8_UP,
} dma_req_id_t;

/* Resolved route for a peripheral request. `name` is the dma device-manager
 * name to acquire from; `stream`/`channel` are the concrete stream index
 * (0..7) and CHSEL value (0..7) the hardware demands. An unknown id returns
 * name=NULL (driver should refuse the transfer). */
typedef struct {
    const char *name;
    uint8_t     stream;
    uint8_t     channel;
} dma_route_t;

dma_route_t dma_hal_route(dma_req_id_t req);

#endif /* DMA_HAL_H */
