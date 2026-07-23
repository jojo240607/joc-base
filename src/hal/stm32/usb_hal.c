#include "usb_hal.h"
#include <stdlib.h>
#include <string.h>

/*
 * STM32F4 OTG FS device-mode HAL. See usb_hal.h for the contract.
 * All register field positions use the CMSIS USB_OTG_*_Pos / _Msk macros.
 */

#define USB_OTG_FS_BASE_ADDR  0x50000000UL
#define USB_OTG_FS_EP_FIFO    0x1000UL          /* TX/RX FIFO array base offset   */
#define USB_OTG_FS_RXFIFO     (USB_OTG_FS_BASE_ADDR + USB_OTG_FS_EP_FIFO)

struct usb_hal_handle {
    USB_OTG_GlobalTypeDef      *global;
    USB_OTG_DeviceTypeDef      *dev;
    USB_OTG_INEndpointTypeDef  *inep[4];
    USB_OTG_OUTEndpointTypeDef *outep[4];
    volatile uint32_t          *rxfifo;          /* shared RxFIFO (read side)      */
    volatile uint32_t          *txfifo[4];        /* per-EP TX FIFOs                */
};

usb_hal_handle_t *usb_hal_create(void *peripheral)
{
    (void)peripheral;   /* OTG FS is at the fixed 0x50000000; arg kept for symmetry */
    usb_hal_handle_t *h = (usb_hal_handle_t *)malloc(sizeof(*h));
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));

    uint32_t base = USB_OTG_FS_BASE_ADDR;
    h->global  = (USB_OTG_GlobalTypeDef *)base;
    h->dev     = (USB_OTG_DeviceTypeDef *)(base + USB_OTG_DEVICE_BASE);
    for (int i = 0; i < 4; i++) {
        h->inep[i]  = (USB_OTG_INEndpointTypeDef  *)(base + USB_OTG_IN_ENDPOINT_BASE  + (uint32_t)i * USB_OTG_EP_REG_SIZE);
        h->outep[i] = (USB_OTG_OUTEndpointTypeDef *)(base + USB_OTG_OUT_ENDPOINT_BASE + (uint32_t)i * USB_OTG_EP_REG_SIZE);
        h->txfifo[i]= (volatile uint32_t *)(base + USB_OTG_FS_EP_FIFO + (uint32_t)i * 0x1000UL);
    }
    h->rxfifo = (volatile uint32_t *)USB_OTG_FS_RXFIFO;
    return h;
}

void usb_hal_destroy(usb_hal_handle_t *h) { free(h); }

void usb_hal_enable_clock(usb_hal_handle_t *h)
{
    (void)h;
    RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
    for (volatile int i = 0; i < 2000; i++) ;   /* let the clock settle */
}

void usb_hal_delay_ms(uint32_t ms)
{
    /* Rough busy delay (~168 MHz). Overshoots; used only for the 25-50 ms
     * force-device-mode settle, where longer is harmless. ~80k iters/ms. */
    volatile uint32_t n = ms * 80000UL;
    while (n--) ;
}

int usb_hal_core_init(usb_hal_handle_t *h, int vbus_sense)
{
    USB_OTG_GlobalTypeDef *g = h->global;

    /* Force DEVICE mode + embedded full-speed PHY. TRDT=9 for a 168 MHz AHB. */
    g->GUSBCFG = USB_OTG_GUSBCFG_FDMOD | USB_OTG_GUSBCFG_PHYSEL |
                 (0x9UL << USB_OTG_GUSBCFG_TRDT_Pos);
    usb_hal_delay_ms(50);                        /* >25 ms before core reset */

    /* Core soft reset, then wait for AHB idle. AHBIDL (bit 31) reads 1 when the
     * AHB master is idle, so we wait UNTIL it is set (loop while it is 0). */
    g->GRSTCTL |= USB_OTG_GRSTCTL_CSRST;
    while (g->GRSTCTL & USB_OTG_GRSTCTL_CSRST) ;
    while (!(g->GRSTCTL & (1UL << 31))) ;        /* AHBIDL: wait for idle */
    usb_hal_delay_ms(5);

    /* Flush FIFOs. */
    g->GRSTCTL = (1UL << 4);                     /* RXFFLSH */
    while (g->GRSTCTL & (1UL << 4)) ;
    g->GRSTCTL = (1UL << 5) | (0x10UL << 6);     /* TXFFLSH, all TxFIFOs */
    while (g->GRSTCTL & (1UL << 5)) ;

    /* FIFO sizing (words). RxFIFO 128; EP0 TX @0x80 (64); EP1 TX @0xC0 (64);
     * EP2 TX @0x100 (32). Leave headroom under the 320-word FIFO RAM. */
    g->GRXFSIZ = 0x80UL;
    g->DIEPTXF0_HNPTXFSIZ = (0x40UL << 16) | 0x80UL;   /* TX0FDEP, TX0FADR */
    g->DIEPTXF[0]         = (0x40UL << 16) | 0xC0UL;   /* EP1 TX */
    g->DIEPTXF[1]         = (0x20UL << 16) | 0x100UL;  /* EP2 TX */

    /* Global interrupt enable. */
    g->GAHBCFG |= USB_OTG_GAHBCFG_GINT;

    /* Power the PHY; ignore VBUS sense so the device "connects" even if the
     * board's VBUS detect is not wired as expected (NOVBUSSENS). */
    g->GCCFG = USB_OTG_GCCFG_PWRDWN;
    if (vbus_sense) g->GCCFG |= USB_OTG_GCCFG_VBUSBSEN;
    else             g->GCCFG |= USB_OTG_GCCFG_NOVBUSSENS;

    /* Device speed = full speed (DSPF[1:0] = 01). */
    h->dev->DCFG = (1UL << USB_OTG_DCFG_DSPD_Pos);

    /* Core + endpoint interrupt masks. */
    g->GINTMSK = USB_HAL_GINT_RXFLVL | USB_HAL_GINT_USBSUSP | USB_HAL_GINT_USBRST |
                 USB_HAL_GINT_ENUMDNE | USB_HAL_GINT_WKUP |
                 USB_HAL_GINT_IEPINT | USB_HAL_GINT_OEPINT;
    h->dev->DIEPMSK = USB_OTG_DIEPMSK_XFRCM;     /* IN transfer complete */
    h->dev->DOEPMSK = USB_OTG_DOEPMSK_XFRCM | USB_OTG_DOEPMSK_STUPM; /* + SETUP */
    h->dev->DAINTMSK = (1UL << 0) | (1UL << 1) | (1UL << 2) |       /* IN EP0,1,2 */
                        (1UL << 16) | (1UL << 17);                   /* OUT EP0,1  */
    return 0;
}

int usb_hal_ep_config(usb_hal_handle_t *h, uint8_t ep, int dir_in,
                      uint16_t mps, usb_ep_type_t type)
{
    if (dir_in) {
        USB_OTG_INEndpointTypeDef *ie = h->inep[ep];
        uint32_t ctl;
        if (ep == 0)   /* EP0 MPSIZ is only 2 bits (00=64) */
            ctl = ((uint32_t)mps & 0x3UL) | (1UL << 15) | (1UL << 26) | (1UL << 31);
        else
            ctl = ((uint32_t)mps & 0x7FFUL) | (1UL << 15) |
                  ((uint32_t)type << USB_OTG_DIEPCTL_EPTYP_Pos) |
                  (1UL << 26) | (1UL << 31);
        ie->DIEPCTL = ctl;
    } else {
        USB_OTG_OUTEndpointTypeDef *oe = h->outep[ep];
        uint32_t ctl;
        if (ep == 0)
            ctl = ((uint32_t)mps & 0x3UL) | (1UL << 15) | (1UL << 26) | (1UL << 31);
        else
            ctl = ((uint32_t)mps & 0x7FFUL) | (1UL << 15) |
                  ((uint32_t)type << USB_OTG_DIEPCTL_EPTYP_Pos) |
                  (1UL << 26) | (1UL << 31);
        oe->DOEPCTL = ctl;
    }
    return 0;
}

int usb_hal_ep_tx(usb_hal_handle_t *h, uint8_t ep, const void *buf, uint16_t len)
{
    USB_OTG_INEndpointTypeDef *ie = h->inep[ep];
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t nwords = (len + 3U) / 4U;
    volatile uint32_t *f = h->txfifo[ep];
    for (uint32_t i = 0; i < nwords; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) {
            int idx = (int)(i * 4U + (uint32_t)b);
            if (idx < (int)len) w |= ((uint32_t)p[idx]) << (b * 8);
        }
        *f = w;
    }
    /* DIEPTSIZ: PKTCNT=1, XFRSIZ=len (EP0 XFRSIZ is 7 bits; len<=64 fits). */
    ie->DIEPTSIZ = (1UL << USB_OTG_DIEPTSIZ_PKTCNT_Pos) | ((uint32_t)len);
    ie->DIEPCTL |= USB_OTG_DIEPCTL_CNAK | USB_OTG_DIEPCTL_EPENA;
    return 0;
}

int usb_hal_ep_rx(usb_hal_handle_t *h, uint8_t ep, uint16_t len)
{
    USB_OTG_OUTEndpointTypeDef *oe = h->outep[ep];
    oe->DOEPTSIZ = (1UL << USB_OTG_DOEPTSIZ_PKTCNT_Pos) | ((uint32_t)len);
    oe->DOEPCTL |= USB_OTG_DOEPCTL_CNAK | USB_OTG_DOEPCTL_EPENA;
    return 0;
}

void usb_hal_fifo_read(usb_hal_handle_t *h, void *buf, uint16_t len)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t nwords = (len + 3U) / 4U;
    volatile uint32_t *f = h->rxfifo;
    for (uint32_t i = 0; i < nwords; i++) {
        uint32_t w = *f;
        for (int b = 0; b < 4; b++) {
            int idx = (int)(i * 4U + (uint32_t)b);
            if (idx < (int)len) p[idx] = (uint8_t)(w >> (b * 8));
        }
    }
}

void usb_hal_ep0_tx_zlp(usb_hal_handle_t *h)
{
    h->inep[0]->DIEPTSIZ = (1UL << USB_OTG_DIEPTSIZ_PKTCNT_Pos);  /* XFRSIZ=0 */
    h->inep[0]->DIEPCTL |= USB_OTG_DIEPCTL_CNAK | USB_OTG_DIEPCTL_EPENA;
}

void usb_hal_ep0_rx_zlp(usb_hal_handle_t *h)
{
    h->outep[0]->DOEPTSIZ = (1UL << USB_OTG_DOEPTSIZ_PKTCNT_Pos) | 64UL;
    h->outep[0]->DOEPCTL |= USB_OTG_DOEPCTL_CNAK | USB_OTG_DOEPCTL_EPENA;
}

uint32_t usb_hal_gintsts(usb_hal_handle_t *h)
{
    return h->global->GINTSTS & h->global->GINTMSK;
}

uint32_t usb_hal_gintsts_raw(usb_hal_handle_t *h)
{
    return h->global->GINTSTS;
}

uint32_t usb_hal_gccfg(usb_hal_handle_t *h)
{
    return h->global->GCCFG;
}

void usb_hal_gint_clear(usb_hal_handle_t *h, uint32_t mask)
{
    h->global->GINTSTS = mask;     /* write-1-to-clear (RXFLVL is read-only) */
}

uint32_t usb_hal_daint(usb_hal_handle_t *h) { return h->dev->DAINT; }

uint32_t usb_hal_rxstsp(usb_hal_handle_t *h) { return h->global->GRXSTSP; }

uint32_t usb_hal_doepint(usb_hal_handle_t *h, uint8_t ep) { return h->outep[ep]->DOEPINT; }
uint32_t usb_hal_diepint(usb_hal_handle_t *h, uint8_t ep) { return h->inep[ep]->DIEPINT; }

void usb_hal_doepint_clear(usb_hal_handle_t *h, uint8_t ep, uint32_t mask)
    { h->outep[ep]->DOEPINT = mask; }
void usb_hal_diepint_clear(usb_hal_handle_t *h, uint8_t ep, uint32_t mask)
    { h->inep[ep]->DIEPINT = mask; }

void usb_hal_connect(usb_hal_handle_t *h)    { h->dev->DCTL &= ~USB_OTG_DCTL_SDIS; }
void usb_hal_disconnect(usb_hal_handle_t *h) { h->dev->DCTL |=  USB_OTG_DCTL_SDIS; }

void usb_hal_set_address(usb_hal_handle_t *h, uint8_t addr)
{
    /* DCFG.DAD = bits [10:4] (mask 0x7F0). */
    uint32_t dcfg = h->dev->DCFG;
    dcfg = (dcfg & ~0x7F0UL) | (((uint32_t)addr << 4) & 0x7F0UL);
    h->dev->DCFG = dcfg;
}

uint32_t usb_hal_dsts(usb_hal_handle_t *h) { return h->dev->DSTS; }

irq_id_t usb_hal_irq_id(usb_hal_handle_t *h)
{
    (void)h;
    return (irq_id_t)OTG_FS_IRQn;     /* 67 on F407 */
}
