/*
 * STM32F4 OTG FS device-mode HAL — thin wrapper over ST's USB device library.
 * See usb_hal.h for the contract.
 */
#include "usb_hal.h"
#include <stdlib.h>
#include <string.h>
#include "stm32f4xx.h"
#include "common/ccm_bss.h"

/* Single OTG FS core instance (ST's stack expects a stable handle).
 * 纯软件核心句柄：OTG FS 用外设内部 FIFO + 内部 DMA 引擎(数据不经过系统 RAM)，
 * 不含 DMA 目标缓冲，搬入 CCM(发布版)安全(开发版已验证 USB CDC PASS)。 */
static USB_OTG_CORE_HANDLE RTOS_CCM_BSS g_pdev;

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

int usb_hal_tx_ep_complete(usb_hal_handle_t *h, uint8_t epnum)
{
    /* SELF-HEAL support: returns 1 if the bulk-IN transfer on `epnum` has FULLY
     * completed at the silicon level — all bytes were drained into the TX FIFO
     * (ST's software xfer_count reached xfer_len) AND the host has read them all
     * (hardware DIEPTSIZ.xfersize == 0) — yet the XFRC interrupt was not observed.
     * When that happens bulk_tx_pending stays 1 forever and TX wedges. The driver
     * uses this to detect a lost-XFRC and safely clear pending. */
    USB_OTG_EP *ep = &h->pdev->dev.in_ep[epnum & 0x7F];
    if (ep->xfer_len == 0)
        return 0;
    if (ep->xfer_count < ep->xfer_len)
        return 0;                                 /* data still being drained into FIFO */
    uint32_t dieptsiz = h->pdev->regs.INEP_REGS[epnum & 0x7F]->DIEPTSIZ;
    if ((dieptsiz & 0x7FFFFUL) != 0)
        return 0;                                 /* host has NOT read all bytes yet */
    return 1;                                     /* transfer done, XFRC presumably lost */
}

int usb_hal_is_suspended(usb_hal_handle_t *h)
{
    /* 1 if the OTG core is currently in SUSPEND (host stopped signalling) — at
     * that point the host is NOT issuing IN tokens, so a pending bulk-IN can
     * never complete (XFRC never fires) and bulk_tx_pending wedges at 1. */
    USB_OTG_DSTS_TypeDef dsts;
    dsts.d32 = h->pdev->regs.DREGS->DSTS;
    return dsts.b.suspsts ? 1 : 0;
}

void usb_hal_remote_wakeup(usb_hal_handle_t *h)
{
    /* Ask the host to resume by pulsing the Remote-Wakeup signalling (only works
     * if the host enabled the REMOTE_WAKEUP feature — most CDC hosts do). The OTG
     * core then drives K-state and the host issues a RESUME, after which it
     * resumes IN-token-ing and the stalled bulk-IN can complete. */
    USB_OTG_ActiveRemoteWakeup(h->pdev);
}
