#ifndef CRC_HAL_H
#define CRC_HAL_H

#include <stdint.h>
#include <stddef.h>

/*
 * Hardware Abstraction Layer — STM32F4 CRC calculation unit (CRC).
 *
 * The ONLY place that touches the CRC registers. The driver layer stays
 * register-free and only calls these functions. The CRC unit needs no external
 * pins and no clock configuration beyond gating it on the AHB1 bus.
 *
 * With the reset-default control bits (no input/output bit reversal) the unit
 * computes CRC-32/MPEG-2: polynomial 0x04C11DB7, init 0xFFFFFFFF, MSB-first,
 * no final XOR. The self-test checks the hardware result against a software
 * implementation of exactly that algorithm.
 */

typedef struct crc_hal_handle crc_hal_handle_t;

crc_hal_handle_t *crc_hal_create(void *peripheral);
void crc_hal_destroy(crc_hal_handle_t *h);

/* Gate the CRC clock (RCC AHB1). Idempotent. */
void crc_hal_enable(crc_hal_handle_t *h);

/* Reset the CRC unit (DR -> 0xFFFFFFFF, ready for a new message). */
void crc_hal_reset(crc_hal_handle_t *h);

/* Feed one 32-bit word into the running CRC. */
void crc_hal_update_u32(crc_hal_handle_t *h, uint32_t word);

/* Feed a block of `nwords` 32-bit words. */
void crc_hal_update(crc_hal_handle_t *h, const uint32_t *buf, size_t nwords);

/* Read the current CRC value. */
uint32_t crc_hal_result(crc_hal_handle_t *h);

#endif /* CRC_HAL_H */
