#include "can_hal.h"
#include "stm32f103xx.h"
#include <stdlib.h>
#include <string.h>

/*
 * Hardware Abstraction Layer — bxCAN (STM32F103).
 *
 * The F103 CAN_TypeDef struct only covers the basic registers (MCR..BTR).
 * TX mailboxes, FIFO mailboxes and filter registers live at fixed offsets
 * beyond BTR, so we access them via pointer arithmetic.
 *
 * CAN1 filter bank 0 offset (same as F4): 0x240
 * TX mailbox 0..2: 0x180, 0x190, 0x1A0
 * FIFO0 mailbox: 0x1B0
 * Filter control registers start at 0x200 (FMR, FM1R, FS1R, FFA1R, FA1R).
 */

#define CAN_TIMEOUT          20000000U
#define CAN_FILTER_BANK0_OFF  0x240U
#define CAN_TX_MBOX0_OFF      0x180U
#define CAN_FIFO0_MBOX_OFF    0x1B0U

/* Filter register offsets — F103 CAN_TypeDef only covers MCR..BTR (0x00..0x1C). */
#define CAN_FMR_OFF     0x200U
#define CAN_FM1R_OFF    0x204U
#define CAN_FS1R_OFF    0x20CU
#define CAN_FFA1R_OFF   0x214U
#define CAN_FA1R_OFF    0x21CU
#define CAN_FMR_FINIT   1U       /* Filter INITialisation mode bit */

/* CAN mailbox register structures (identical layout to F4). */
typedef struct {
    volatile uint32_t TIR;
    volatile uint32_t TDTR;
    volatile uint32_t TDLR;
    volatile uint32_t TDHR;
} can_tx_mailbox_t;

typedef struct {
    volatile uint32_t RIR;
    volatile uint32_t RDTR;
    volatile uint32_t RDLR;
    volatile uint32_t RDHR;
} can_fifo_mailbox_t;

typedef struct {
    volatile uint32_t FR1;
    volatile uint32_t FR2;
} can_filter_reg_t;

struct can_hal_handle {
    CAN_TypeDef *reg;
    uint32_t     pclk_hz;
    uint32_t     presc;
    uint32_t     sjw;
    uint32_t     bs1;
    uint32_t     bs2;
    int          loopback;
    int          silent;
    int          remap;
};

can_hal_handle_t *can_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    can_hal_handle_t *h = (can_hal_handle_t *)malloc(sizeof(can_hal_handle_t));
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));
    h->reg = (CAN_TypeDef *)peripheral;
    return h;
}

void can_hal_destroy(can_hal_handle_t *h) { free(h); }

void can_hal_enable_clock(can_hal_handle_t *h)
{
    if (!h) return;
    void *p = (void *)h->reg;
    if (p == (void *)CAN1_BASE)
        RCC->APB1ENR |= RCC_APB1ENR_CAN1EN;
}

int can_hal_init(can_hal_handle_t *h, uint32_t pclk_hz,
                 uint32_t presc, uint32_t sjw, uint32_t bs1, uint32_t bs2,
                 int loopback, int silent, int remap)
{
    if (!h) return -1;
    h->pclk_hz  = pclk_hz;
    h->presc    = presc;
    h->sjw      = sjw;
    h->bs1      = bs1;
    h->bs2      = bs2;
    h->loopback = loopback;
    h->silent   = silent;
    h->remap    = remap;

    CAN_TypeDef *r = h->reg;
    uint32_t tmo;

    /* Request initialisation mode (INRQ=1, SLEEP=0). */
    r->MCR = CAN_MCR_INRQ;
    tmo = CAN_TIMEOUT;
    while ((r->MSR & CAN_MSR_INAK) == 0) { if (--tmo == 0) return -1; }

    /* Configure bit timing + loopback/silent */
    uint32_t btr = ((presc - 1U) << 20)
                 | ((sjw   - 1U) << 16)
                 | ((bs1   - 1U) << 8)
                 | ((bs2   - 1U) << 0);
    if (loopback) btr |= (1u << 30);   /* LBKM */
    if (silent)   btr |= (1u << 31);   /* SILM */
    r->BTR = btr;

    /* Leave init mode (INRQ=0) — enter normal mode. */
    r->MCR = 0;
    tmo = CAN_TIMEOUT;
    while ((r->MSR & CAN_MSR_INAK) != 0) { if (--tmo == 0) return -1; }

    /* Configure filter bank 0: 32-bit MASK = accept-all, assign to FIFO0. */
    volatile uint32_t *fmr   = (volatile uint32_t *)((uint32_t)r + CAN_FMR_OFF);
    volatile uint32_t *fm1r  = (volatile uint32_t *)((uint32_t)r + CAN_FM1R_OFF);
    volatile uint32_t *fs1r  = (volatile uint32_t *)((uint32_t)r + CAN_FS1R_OFF);
    volatile uint32_t *ffa1r = (volatile uint32_t *)((uint32_t)r + CAN_FFA1R_OFF);
    volatile uint32_t *fa1r  = (volatile uint32_t *)((uint32_t)r + CAN_FA1R_OFF);
    *fmr |= CAN_FMR_FINIT;
    /* Filter 0: FM1R bit0=0 (mask mode), FS1R bit0=1 (32-bit scale),
     * FFA1R bit0=0 (FIFO0), FA1R bit0=1 (activate). */
    can_filter_reg_t *f = (can_filter_reg_t *)((uint32_t)r + CAN_FILTER_BANK0_OFF);
    f->FR1 = 0U;                       /* mask = 0 (accept all) */
    f->FR2 = 0U;                       /* id = 0 (all frames) */
    /* Clear filter bits for bank 0 */
    *fm1r  &= ~(1u << 0);              /* mask mode */
    *fs1r  |=  (1u << 0);              /* 32-bit width */
    *ffa1r &= ~(1u << 0);              /* FIFO0 */
    *fa1r  |=  (1u << 0);              /* activate */
    *fmr   &= ~CAN_FMR_FINIT;
    return 0;
}

int can_hal_send(can_hal_handle_t *h, const can_frame_t *f, uint32_t timeout)
{
    if (!h || !f || f->dlc > 8U) return -1;
    (void)timeout;  /* unused on F103 */

    CAN_TypeDef *r = h->reg;
    volatile uint32_t tmo = CAN_TIMEOUT;
    int mb = -1;

    while (1) {
        uint32_t tsr = r->TSR;
        if (tsr & (1u << 26)) { mb = 0; break; }
        if (tsr & (1u << 27)) { mb = 1; break; }
        if (tsr & (1u << 28)) { mb = 2; break; }
        if (--tmo == 0) return -1;
    }

    can_tx_mailbox_t *m = (can_tx_mailbox_t *)((uint32_t)r + CAN_TX_MBOX0_OFF + mb * 0x10U);

    r->TSR = (1u << (mb * 8 + 0));       /* clear stale RQCP */
    m->TIR  = (f->id & 0x7FFU) << 21;    /* standard ID */
    if (f->ext) m->TIR |= (1u << 2);     /* IDE */
    if (f->rtr) m->TIR |= (1u << 1);     /* RTR */
    m->TDTR = (uint32_t)(f->dlc & 0xFU);
    /* Copy data bytes into TDLR / TDHR */
    uint32_t dl = 0, dh = 0;
    uint32_t i;
    for (i = 0; i < 4 && i < f->dlc; i++) dl |= ((uint32_t)f->data[i] << (i * 8));
    for (i = 0; i < 4 && (i + 4) < f->dlc; i++) dh |= ((uint32_t)f->data[i + 4] << (i * 8));
    m->TDLR = dl;
    m->TDHR = dh;
    m->TIR |= (1u << 0);                  /* TXRQ */

    uint32_t rqcp = (1u << (mb * 8 + 0));
    uint32_t txok = (1u << (mb * 8 + 1));
    tmo = CAN_TIMEOUT;
    while (!(r->TSR & rqcp)) { if (--tmo == 0) return -1; }
    if (!(r->TSR & txok)) return -1;
    return 0;
}

int can_hal_recv(can_hal_handle_t *h, can_frame_t *f, uint32_t timeout)
{
    if (!h || !f) return -1;
    (void)timeout;

    CAN_TypeDef *r = h->reg;
    volatile uint32_t tmo = CAN_TIMEOUT;
    while ((r->RF0R & CAN_RF0R_FMP0) == 0) { if (--tmo == 0) return -1; }

    can_fifo_mailbox_t *mbx = (can_fifo_mailbox_t *)((uint32_t)r + CAN_FIFO0_MBOX_OFF);

    uint32_t rir  = mbx->RIR;
    uint32_t rdtr = mbx->RDTR;
    uint32_t rdlr = mbx->RDLR;
    uint32_t rdhr = mbx->RDHR;

    f->dlc = rdtr & 0xFU;
    f->ext = (rir >> 2) & 1U;
    f->rtr = (rir >> 1) & 1U;
    f->id  = (rir >> 21) & 0x7FFU;

    f->data[0] = (uint8_t)(rdlr & 0xFFU);
    f->data[1] = (uint8_t)((rdlr >> 8) & 0xFFU);
    f->data[2] = (uint8_t)((rdlr >> 16) & 0xFFU);
    f->data[3] = (uint8_t)((rdlr >> 24) & 0xFFU);
    f->data[4] = (uint8_t)(rdhr & 0xFFU);
    f->data[5] = (uint8_t)((rdhr >> 8) & 0xFFU);
    f->data[6] = (uint8_t)((rdhr >> 16) & 0xFFU);
    f->data[7] = (uint8_t)((rdhr >> 24) & 0xFFU);

    r->RF0R |= CAN_RF0R_RFOM0;
    return 0;
}

/* Diagnostic getters — direct register readback */
uint32_t can_hal_get_mcr(can_hal_handle_t *h)  { return h ? h->reg->MCR  : 0; }
uint32_t can_hal_get_btr(can_hal_handle_t *h)  { return h ? h->reg->BTR  : 0; }
uint32_t can_hal_get_msr(can_hal_handle_t *h)  { return h ? h->reg->MSR  : 0; }
uint32_t can_hal_get_esr(can_hal_handle_t *h)  { return h ? h->reg->ESR  : 0; }
uint32_t can_hal_get_tsr(can_hal_handle_t *h)  { return h ? h->reg->TSR  : 0; }
uint32_t can_hal_get_rf0r(can_hal_handle_t *h) { return h ? h->reg->RF0R : 0; }
uint32_t can_hal_get_fmr(can_hal_handle_t *h)  { if (!h) return 0; return *(volatile uint32_t *)((uint32_t)h->reg + CAN_FMR_OFF);  }
uint32_t can_hal_get_fa1r(can_hal_handle_t *h) { if (!h) return 0; return *(volatile uint32_t *)((uint32_t)h->reg + CAN_FA1R_OFF); }