#ifndef DAC_HAL_H
#define DAC_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — DAC (STM32F4 DAC1, channels 1/2).
 * All register knowledge lives here; the driver only sees the opaque handle.
 */
typedef struct dac_hal_handle dac_hal_handle_t;

dac_hal_handle_t *dac_hal_create(void *peripheral);   /* DAC_TypeDef* (board only) */
void dac_hal_destroy(dac_hal_handle_t *h);

void dac_hal_enable_clock(dac_hal_handle_t *h);
/* Enable one channel (ch=1|2): ENx=1, trigger disabled (TENx=0 -> DHR auto-
 * transfers to DOR), no waveform, output buffer on. */
void dac_hal_enable_channel(dac_hal_handle_t *h, int ch);
/* Write a 12-bit right-aligned value into the channel's DHR12R register. */
void dac_hal_write(dac_hal_handle_t *h, int ch, uint16_t val);
/* Read back the channel's DOR register (the value latched to the output). */
uint16_t dac_hal_get_dor(dac_hal_handle_t *h, int ch);
/* Read back the channel's DHR register (diagnostic). */
uint16_t dac_hal_get_dhr(dac_hal_handle_t *h, int ch);
/* Read back CR (for verification). */
uint32_t dac_hal_get_cr(dac_hal_handle_t *h);

/* --- DMA support (used by the driver's STREAM_MODE_DMA engine) ---
 * The STM32F4 DAC channels are hard-wired to DMA streams (DAC1->DMA1_Stream5).
 * The driver resolves that stream via dma_hal_route() and drives it through the
 * generic dma device; the HAL only exposes the DHR data-register address and the
 * CR DMAEN bits that gate the DAC's DMA request. */
void    *dac_hal_get_dhr_addr(dac_hal_handle_t *h, int ch); /* (void*)&DHR12Rx — DMA PAR */
void     dac_hal_enable_dma(dac_hal_handle_t *h, int ch);   /* CR DMAENx=1 */
void     dac_hal_disable_dma(dac_hal_handle_t *h, int ch);  /* CR DMAENx=0 */

/* --- Trigger mode (REQUIRED for DAC+DMA on STM32F4) ---
 * In static mode (TENx=0) the DAC never raises a DMA request, so a DMA burst
 * starves. Trigger mode (TENx=1) makes each selected trigger edge move DHR->DOR
 * and raise the DMA request, so the stream is clocked by a timer TRGO. tsel is
 * the 3-bit CR TSEL field (0 = TIM6 TRGO, 2 = TIM7 TRGO, ...). */
void dac_hal_enable_trigger(dac_hal_handle_t *h, int ch, uint32_t tsel);
void dac_hal_disable_trigger(dac_hal_handle_t *h, int ch);   /* back to static */

#endif /* DAC_HAL_H */
