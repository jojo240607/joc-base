#ifndef CAN_HAL_H
#define CAN_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — bxCAN (STM32F4).
 *
 * The STM32F4 has a "bxCAN" controller: CAN1 is the MASTER (owns the filter
 * banks + the shared filter master register FMR), CAN2 is the SLAVE (shares the
 * filter banks). This HAL drives CAN1 only and keeps ALL register knowledge
 * here — the driver above sees only the opaque handle + can_frame_t.
 *
 * For a board with NO CAN transceiver (the Discovery board has none), the
 * controller is put in SILENT + LOOPBACK mode: the transmitted frame is looped
 * back internally to the receive FIFO (proving the TX and RX data paths) while
 * the Tx pin stays tri-stated (so nothing is driven onto a non-existent bus).
 * This makes the CAN driver fully self-testable with zero external hardware.
 *
 * bxCAN bit timing: one CAN bit = (1 + BS1 + BS2) time quanta (tq);
 * tq = prescaler / PCLK1. The sample point is at the end of BS1.
 */
typedef struct can_hal_handle can_hal_handle_t;

/* A single CAN 2.0 frame (std or extended). */
typedef struct {
    uint32_t id;       /* 11-bit std id, or 29-bit ext id (no IDE bit — use `ext`) */
    uint8_t  ext;      /* 1 = extended 29-bit id, 0 = standard 11-bit */
    uint8_t  rtr;      /* 1 = remote transmission request (no data) */
    uint8_t  dlc;      /* data length code: number of data bytes 0..8 */
    uint8_t  data[8];  /* payload */
} can_frame_t;

can_hal_handle_t *can_hal_create(void *peripheral);
void can_hal_destroy(can_hal_handle_t *h);

/* Enable the CAN peripheral clock (APB1 for CAN1/CAN2). */
void can_hal_enable_clock(can_hal_handle_t *h);

/* Configure + start the bxCAN.
 *   pclk_hz : APB1 clock (e.g. 42 MHz) — used for diagnostics only
 *   presc   : baud-rate prescaler (BRP = presc-1), >= 1
 *   sjw     : (re)synchronization jump width in tq (SJW = sjw-1), >= 1
 *   bs1     : time segment 1 in tq (TS1 = bs1-1), >= 1
 *   bs2     : time segment 2 in tq (TS2 = bs2-1), >= 1
 *   loopback: 1 => LBKM (internal loopback, no transceiver needed)
 *   silent  : 1 => SILM (Tx pin tri-stated, never drives a bus)
 * Configures filter bank 0 as 32-bit MASK = accept-all and assigns it to FIFO0.
 * Returns 0 on success, -1 on timeout leaving init mode. */
int can_hal_init(can_hal_handle_t *h, uint32_t pclk_hz,
                 uint32_t presc, uint32_t sjw, uint32_t bs1, uint32_t bs2,
                 int loopback, int silent, int remap);

/* Transmit one frame. Returns 0 on TXOK, -1 on timeout / no free mailbox. */
int can_hal_send(can_hal_handle_t *h, const can_frame_t *f, uint32_t timeout);

/* Receive one frame from FIFO0. Returns 0 on success, -1 on empty / timeout. */
int can_hal_recv(can_hal_handle_t *h, can_frame_t *f, uint32_t timeout);

/* readback getters for self-test verification (all return raw register words) */
uint32_t can_hal_get_mcr(can_hal_handle_t *h);   /* CAN_MCR */
uint32_t can_hal_get_btr(can_hal_handle_t *h);   /* CAN_BTR (LBKM/SILM/timing) */
uint32_t can_hal_get_msr(can_hal_handle_t *h);   /* CAN_MSR (INAK/SLAK) */
uint32_t can_hal_get_esr(can_hal_handle_t *h);   /* CAN_ESR (error/status) */
uint32_t can_hal_get_tsr(can_hal_handle_t *h);   /* CAN_TSR (tx mailbox status) */
uint32_t can_hal_get_rf0r(can_hal_handle_t *h);  /* CAN_RF0R (rx fifo 0) */
uint32_t can_hal_get_fmr(can_hal_handle_t *h);   /* CAN_FMR (filter master) */
uint32_t can_hal_get_fa1r(can_hal_handle_t *h);  /* CAN_FA1R (filter active) */

#endif /* CAN_HAL_H */
