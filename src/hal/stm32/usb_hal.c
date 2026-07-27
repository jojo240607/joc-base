/*
 * STM32F4 OTG FS device-mode HAL — thin wrapper over ST's USB device library.
 * See usb_hal.h for the contract.
 */
#include "usb_hal.h"
#include <stdlib.h>
#include <string.h>
#include "stm32f4xx.h"

/* Single OTG FS core instance (ST's stack expects a stable handle). */
static USB_OTG_CORE_HANDLE g_pdev;

/* OTG internal-DMA default ON (see usb_hal.h). The driver may clear it from
 * its board config; ST's USB_OTG_SelectCore reads it when building the core
 * cfg at init time. */
static int g_otg_dma_enable = 1;

void usb_hal_set_dma_enable(int on) { g_otg_dma_enable = on ? 1 : 0; }
int  usb_hal_get_dma_enable(void)   { return g_otg_dma_enable; }

usb_hal_handle_t *usb_hal_create(void *peripheral)
{
    (void)peripheral;   /* OTG FS is at the fixed 0x50000000; arg kept for symmetry */
    usb_hal_handle_t *h = (usb_hal_handle_t *)malloc(sizeof(*h));
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));
    h->pdev = &g_pdev;
    return h;
}

void usb_hal_destroy(usb_hal_handle_t *h) { free(h); }

USB_OTG_CORE_HANDLE *usb_hal_pdev(usb_hal_handle_t *h) { return h->pdev; }

void usb_hal_enable_clock(usb_hal_handle_t *h)
{
    (void)h;
    RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
    for (volatile int i = 0; i < 2000; i++) ;   /* let the clock settle */
}

void usb_hal_connect(usb_hal_handle_t *h)    { DCD_DevConnect(h->pdev); }
void usb_hal_disconnect(usb_hal_handle_t *h) { DCD_DevDisconnect(h->pdev); }

irq_id_t usb_hal_irq_id(usb_hal_handle_t *h)
{
    (void)h;
    return (irq_id_t)OTG_FS_IRQn;     /* 67 on F407 */
}

uint32_t usb_hal_gintsts_raw(usb_hal_handle_t *h) { return h->pdev->regs.GREGS->GINTSTS; }
uint32_t usb_hal_gccfg(usb_hal_handle_t *h)        { return h->pdev->regs.GREGS->GCCFG; }
uint32_t usb_hal_dcfg(usb_hal_handle_t *h)         { return h->pdev->regs.DREGS->DCFG; }
uint32_t usb_hal_dsts(usb_hal_handle_t *h)         { return h->pdev->regs.DREGS->DSTS; }
uint32_t usb_hal_dctl(usb_hal_handle_t *h)         { return h->pdev->regs.DREGS->DCTL; }
uint32_t usb_hal_gusbcfg(usb_hal_handle_t *h)      { return h->pdev->regs.GREGS->GUSBCFG; }
uint32_t usb_hal_diepctl(usb_hal_handle_t *h, uint8_t ep) { return h->pdev->regs.INEP_REGS[ep]->DIEPCTL; }
uint32_t usb_hal_doepctl(usb_hal_handle_t *h, uint8_t ep) { return h->pdev->regs.OUTEP_REGS[ep]->DOEPCTL; }
