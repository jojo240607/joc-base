#include "can_hal.h"
#include "stm32f4xx.h"     /* CAN_TypeDef, RCC, CAN1_BASE + bxCAN bit defs */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Hardware Abstraction Layer — bxCAN (STM32F4).
 *
 * Register bit positions are spelled out with literals (mirroring the CMSIS
 * #defines) so the HAL does not depend on a particular header revision.
 */

/* Per-wait timeout (tight-loop iterations) for mailbox / fifo polling. Generous:
 * leaving init mode needs 11 recessive bits on the (loopback) bus; give it time. */
#define CAN_TIMEOUT  20000000U

/* Register bit positions reused from the CMSIS header (stm32f407xx.h):
 *   CAN_MCR_INRQ / CAN_MCR_SLEEP, CAN_MSR_INAK / CAN_MSR_SLAK,
 *   CAN_BTR_LBKM / CAN_BTR_SILM, CAN_RF0R_FMP0 / CAN_RF0R_RFOM0.
 * The filter-bank registers live in a CAN_FilterRegister_TypeDef block that
 * begins at offset 0x240 inside the CAN peripheral (this trimmed CMSIS does
 * not expose it as a struct member), so we reach bank 0 through a cast. */
#define CAN_FILTER_BANK0_OFF  0x240U

struct can_hal_handle {
    CAN_TypeDef *reg;
    uint32_t     pclk_hz;
    uint32_t     presc;
    uint32_t     sjw;
    uint32_t     bs1;
    uint32_t     bs2;
    int          loopback;
    int          silent;
    int          remap;     /* unused on F4 (CAN pins chosen via GPIO AF matrix) */
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
    else if (p == (void *)CAN2_BASE)
        RCC->APB1ENR |= RCC_APB1ENR_CAN2EN;
    printf("[can_hal] clock: APB1ENR=0x%08lX CAN1EN=%d CAN_PCLK=%lu\r\n",
           (unsigned long)RCC->APB1ENR,
           (int)((RCC->APB1ENR & RCC_APB1ENR_CAN1EN) != 0U),
           (unsigned long)h->pclk_hz);
}

int can_hal_init(can_hal_handle_t *h, uint32_t pclk_hz,
                 uint32_t presc, uint32_t sjw, uint32_t bs1, uint32_t bs2,
                 int loopback, int silent, int remap)
{
    if (!h) return -1;
    CAN_TypeDef *r = h->reg;

    h->pclk_hz  = pclk_hz;
    h->presc    = presc  < 1U ? 1U : presc;
    h->sjw      = sjw    < 1U ? 1U : sjw;
    h->bs1      = bs1    < 1U ? 1U : bs1;
    h->bs2      = bs2    < 1U ? 1U : bs2;
    h->loopback = loopback ? 1 : 0;
    h->silent   = silent   ? 1 : 0;
    h->remap    = remap   ? 1 : 0;

    /* On STM32F4 the CAN1_RX/TX pins are selected PURELY by the GPIO AF matrix
     * (PB8/PB9 = AF9 = CAN1_RX/TX) — there is NO SYSCFG CAN-remap bit (that
     * register exists only on F0/F3/L0). The Discovery board wires PA11/PA12 to
     * the USB-OTG connector, so the default CAN1_RX (PA11) is held DOMINANT by
     * board hardware and bxCAN can never count 11 recessive bits to leave init
     * mode. We therefore use the alternate free pins PB8(RX)/PB9(TX), AF9, which
     * the board layer routes CAN1 to via the pinmux (PB8/PB9 are free here;
     * I2C1 is on PB6/PB7). With PB8 pulled up the (loopback) bus is idle-
     * recessive, so the controller CAN leave init mode. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;

    /* DIAG (NON-DESTRUCTIVE): the pinmux already configured PB8/PB9 as AF9
     * before can_hal_init() was called, so just read the live pin level to
     * confirm PB8 is recessive (idle). Do NOT touch MODER/PUPDR here or we would
     * disconnect the AF mux and the controller would lose its RX pin. */
    volatile uint32_t pb8_raw = (GPIOB->IDR >> 8) & 1U;
    volatile uint32_t pb9_raw = (GPIOB->IDR >> 9) & 1U;
    printf("[can_hal] PB8/PB9 level(AF9)=%lu/%lu PUPDR=0x%08lX\r\n",
           (unsigned long)pb8_raw, (unsigned long)pb9_raw, (unsigned long)GPIOB->PUPDR);

    /* 1) Enter initialisation mode: MCR.INRQ=1 (full write, clears SLEEP/DBF/etc),
     *    wait MSR.INAK. A full write matches the canonical HAL sequence and avoids
     *    leaving a stray debug-freeze (DBF) bit set, which would freeze the CAN
     *    bit engine and prevent INAK from ever clearing. */
    r->MCR = CAN_MCR_INRQ;
    printf("[can_hal] enter-init MCR=0x%08lX MSR=0x%08lX\r\n",
           (unsigned long)r->MCR, (unsigned long)r->MSR);
    volatile uint32_t tmo = CAN_TIMEOUT;
    while (!(r->MSR & CAN_MSR_INAK)) { if (--tmo == 0) return -1; }
    printf("[can_hal] entered-init tmo=%lu MSR=0x%08lX\r\n",
           (unsigned long)tmo, (unsigned long)r->MSR);

    /* 2) Master (CAN1) filter config: bank 0 = 32-bit MASK, accept-all, FIFO0.
     *    Filter banks are configured under FINIT (CAN_FMR.FINIT=1); the writes
     *    only latch when FINIT is set, otherwise the looped-back frame is
     *    silently dropped and the RX FIFO stays empty. */
    r->FMR |= (1u << 0);                   /* FINIT = 1: filter init mode */
    r->FA1R &= ~(1u << 0);                 /* deactivate bank 0 */
    r->FM1R &= ~(1u << 0);                 /* mask mode (not list) */
    r->FS1R |=  (1u << 0);                 /* single 32-bit scale */
    r->FFA1R &= ~(1u << 0);                /* assign to FIFO 0 */
    CAN_FilterRegister_TypeDef *fr = (CAN_FilterRegister_TypeDef *)
        ((uint32_t)r + CAN_FILTER_BANK0_OFF);
    fr->FR1 = 0x00000000U;                 /* id = 0 */
    fr->FR2 = 0x00000000U;                 /* mask = 0 => accept all */
    r->FA1R |=  (1u << 0);                 /* activate bank 0 */
    r->FMR &= ~(1u << 0);                  /* FINIT = 0: filters live */

    /* 3) Program the bit-timing register (leave init fields, add lbkm/silm). */
    uint32_t btr = (((h->sjw - 1U) & 0x3U)  << 24)
                 | (((h->bs2 - 1U) & 0x7U)  << 20)
                 | (((h->bs1 - 1U) & 0xFU)  << 16)
                 | (((h->presc - 1U) & 0x3FFU) << 0);
    if (h->loopback) btr |= CAN_BTR_LBKM;
    if (h->silent)   btr |= CAN_BTR_SILM;
    r->BTR = btr;

    /* 4) Leave initialisation mode: MCR.INRQ=0 (full write), wait MSR.INAK cleared. */
    r->MCR = 0U;
    printf("[can_hal] leave-init MCR=0x%08lX MSR=0x%08lX BTR=0x%08lX\r\n",
           (unsigned long)r->MCR, (unsigned long)r->MSR, (unsigned long)r->BTR);
    tmo = CAN_TIMEOUT;
    while ((r->MSR & CAN_MSR_INAK)) { if (--tmo == 0) {
        printf("[can_hal] EXIT-INIT TIMEOUT ESR=0x%08lX MSR=0x%08lX BTR=0x%08lX "
               "PB8_IDR=%lu PUPDR=0x%08lX\r\n",
               (unsigned long)r->ESR, (unsigned long)r->MSR, (unsigned long)r->BTR,
               (unsigned long)((GPIOB->IDR >> 8) & 1U), (unsigned long)GPIOB->PUPDR);
        return -1;
    } }
    printf("[can_hal] left-init tmo=%lu MSR=0x%08lX\r\n",
           (unsigned long)tmo, (unsigned long)r->MSR);

    return 0;
}

/* pick the first transmit mailbox that is empty (TME flag). */
static int can_hal_free_mbox(can_hal_handle_t *h)
{
    uint32_t tsr = h->reg->TSR;
    if (tsr & (1u << 26)) return 0;   /* TME0 */
    if (tsr & (1u << 27)) return 1;   /* TME1 */
    if (tsr & (1u << 28)) return 2;   /* TME2 */
    return -1;
}

int can_hal_send(can_hal_handle_t *h, const can_frame_t *f, uint32_t timeout)
{
    if (!h || !f) return -1;
    if (f->dlc > 8U) return -1;

    int mb = can_hal_free_mbox(h);
    if (mb < 0) {
        /* wait for a mailbox to free up */
        volatile uint32_t tmo = timeout ? timeout : CAN_TIMEOUT;
        while ((mb = can_hal_free_mbox(h)) < 0) { if (--tmo == 0) return -1; }
    }

    CAN_TypeDef *r = h->reg;
    CAN_TxMailBox_TypeDef *m = &r->sTxMailBox[mb];

    /* Clear any stale RQCP flag for this mailbox (write-1-to-clear) so the
     * completion wait below is a real wait, not an immediate exit on a flag
     * left set from a previous transfer. */
    r->TSR = (1u << (mb * 8 + 0));

    /* Identifier register. */
    uint32_t tir = 0;
    if (f->ext) {
        tir |= (1u << 2);                       /* IDE = extended */
        tir |= (f->id & 0x1FFFFFFFU) << 3;
    } else {
        tir |= (f->id & 0x7FFU) << 21;          /* STID[10:0] */
    }
    if (f->rtr) tir |= (1u << 1);                /* RTR */

    m->TIR  = tir;
    m->TDTR = (uint32_t)f->dlc;
    m->TDLR = ((uint32_t)f->data[0])
            | ((uint32_t)f->data[1] << 8)
            | ((uint32_t)f->data[2] << 16)
            | ((uint32_t)f->data[3] << 24);
    m->TDHR = ((uint32_t)f->data[4])
            | ((uint32_t)f->data[5] << 8)
            | ((uint32_t)f->data[6] << 16)
            | ((uint32_t)f->data[7] << 24);

    /* Request transmission. */
    m->TIR |= (1u << 0);                         /* TXRQ */

    /* Wait for RQCP + TXOK on this mailbox. TSR mailbox-n layout:
     *   bit (mb*8 + 0) = RQCPn (request completed)
     *   bit (mb*8 + 1) = TXOKn  (transmission ok)   <-- NOT bit +7 (that is TERRn)
     *   bit (mb*8 + 3) = TERRn  (transmission error) */
    uint32_t rqcp = (1u << (mb * 8 + 0));
    uint32_t txok = (1u << (mb * 8 + 1));
    volatile uint32_t tmo = timeout ? timeout : CAN_TIMEOUT;
    while (!(r->TSR & rqcp)) { if (--tmo == 0) return -1; }
    if (!(r->TSR & txok)) return -1;
    return 0;
}

int can_hal_recv(can_hal_handle_t *h, can_frame_t *f, uint32_t timeout)
{
    if (!h || !f) return -1;
    CAN_TypeDef *r = h->reg;

    /* Wait for at least one message in FIFO0. */
    volatile uint32_t tmo = timeout ? timeout : CAN_TIMEOUT;
    while ((r->RF0R & CAN_RF0R_FMP0) == 0) { if (--tmo == 0) return -1; }

    CAN_FIFOMailBox_TypeDef *mbx = &r->sFIFOMailBox[0];

    uint32_t rir  = mbx->RIR;
    uint32_t rdtr = mbx->RDTR;
    uint32_t rdlr = mbx->RDLR;
    uint32_t rdhr = mbx->RDHR;

    f->dlc = (uint8_t)(rdtr & 0xFu);
    if (rir & (1u << 2)) {                       /* IDE = extended */
        f->ext = 1;
        f->id  = (rir >> 3) & 0x1FFFFFFFU;
    } else {
        f->ext = 0;
        f->id  = (rir >> 21) & 0x7FFU;
    }
    f->rtr = (rir & (1u << 1)) ? 1 : 0;

    f->data[0] = (uint8_t)(rdlr & 0xFFU);
    f->data[1] = (uint8_t)((rdlr >> 8) & 0xFFU);
    f->data[2] = (uint8_t)((rdlr >> 16) & 0xFFU);
    f->data[3] = (uint8_t)((rdlr >> 24) & 0xFFU);
    f->data[4] = (uint8_t)(rdhr & 0xFFU);
    f->data[5] = (uint8_t)((rdhr >> 8) & 0xFFU);
    f->data[6] = (uint8_t)((rdhr >> 16) & 0xFFU);
    f->data[7] = (uint8_t)((rdhr >> 24) & 0xFFU);

    /* Release the FIFO output mailbox so the next frame can be read. */
    r->RF0R |= CAN_RF0R_RFOM0;
    return 0;
}

uint32_t can_hal_get_mcr(can_hal_handle_t *h)  { return h ? h->reg->MCR  : 0UL; }
uint32_t can_hal_get_btr(can_hal_handle_t *h)  { return h ? h->reg->BTR  : 0UL; }
uint32_t can_hal_get_msr(can_hal_handle_t *h)  { return h ? h->reg->MSR  : 0UL; }
uint32_t can_hal_get_esr(can_hal_handle_t *h)  { return h ? h->reg->ESR  : 0UL; }
uint32_t can_hal_get_tsr(can_hal_handle_t *h)  { return h ? h->reg->TSR  : 0UL; }
uint32_t can_hal_get_rf0r(can_hal_handle_t *h) { return h ? h->reg->RF0R : 0UL; }
uint32_t can_hal_get_fmr(can_hal_handle_t *h)  { return h ? h->reg->FMR  : 0UL; }
uint32_t can_hal_get_fa1r(can_hal_handle_t *h) { return h ? h->reg->FA1R : 0UL; }
