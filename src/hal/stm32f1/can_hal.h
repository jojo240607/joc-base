#ifndef CAN_HAL_H
#define CAN_HAL_H

#include <stdint.h>
#include "irq.h"

/*
 * Hardware Abstraction Layer — bxCAN (STM32F103).
 *
 * The F103 bxCAN has identical register layout to F4 but the CAN_TypeDef struct
 * only covers the base registers (MCR..BTR). TX mailboxes, FIFO mailboxes and
 * filter registers live at fixed offsets beyond BTR, accessed via pointer
 * arithmetic in the .c file.
 *
 * CAN1 only (F103 has two CAN controllers but no CAN2 on some variants).
 */
typedef struct can_hal_handle can_hal_handle_t;

typedef struct {
    uint32_t id;       /* 11-bit std id, or 29-bit ext id (no IDE bit — use `ext`) */
    uint8_t  ext;      /* 1 = extended 29-bit id, 0 = standard 11-bit */
    uint8_t  rtr;      /* 1 = remote transmission request (no data) */
    uint8_t  dlc;      /* data length code: number of data bytes 0..8 */
    uint8_t  data[8];  /* payload */
} can_frame_t;

can_hal_handle_t *can_hal_create(void *peripheral);
void can_hal_destroy(can_hal_handle_t *h);

void can_hal_enable_clock(can_hal_handle_t *h);

int can_hal_init(can_hal_handle_t *h, uint32_t pclk_hz,
                 uint32_t presc, uint32_t sjw, uint32_t bs1, uint32_t bs2,
                 int loopback, int silent, int remap);

int can_hal_send(can_hal_handle_t *h, const can_frame_t *f, uint32_t timeout);
int can_hal_recv(can_hal_handle_t *h, can_frame_t *f, uint32_t timeout);

/* diagnostic getters (for self-test verification) */
uint32_t can_hal_get_mcr(can_hal_handle_t *h);
uint32_t can_hal_get_btr(can_hal_handle_t *h);
uint32_t can_hal_get_msr(can_hal_handle_t *h);
uint32_t can_hal_get_esr(can_hal_handle_t *h);
uint32_t can_hal_get_tsr(can_hal_handle_t *h);
uint32_t can_hal_get_rf0r(can_hal_handle_t *h);
uint32_t can_hal_get_fmr(can_hal_handle_t *h);
uint32_t can_hal_get_fa1r(can_hal_handle_t *h);

#endif /* CAN_HAL_H */