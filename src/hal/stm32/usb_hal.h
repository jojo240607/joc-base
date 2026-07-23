#ifndef USB_HAL_H
#define USB_HAL_H

#include <stdint.h>
#include <stddef.h>
#include "stm32f4xx.h"     /* USB_OTG_* register structs + bit masks (CMSIS) */
#include "irq_hal.h"       /* irq_id_t */

/*
 * Hardware Abstraction Layer — STM32F4 OTG FS, DEVICE (peripheral) mode.
 *
 * Register layout and bit positions come straight from the (trimmed) CMSIS
 * header stm32f407xx.h, so there are NO hand-computed shifts here — every
 * field is spelled with the USB_OTG_*_Pos / _Msk macros. The driver (drv/usb.c)
 * owns the CDC protocol; this HAL owns only the silicon: core reset, PHY/FIFO
 * setup, endpoint priming, FIFO word push/pop, and interrupt-status access.
 *
 * Endpoint usage for the CDC function (see drv/usb.c):
 *   EP0  IN+OUT  control (enumerate)
 *   EP1  IN+OUT  CDC data bulk (the VCP byte stream)
 *   EP2  IN      CDC ACM notification (interrupt)
 */

typedef enum {
    USB_EP_TYPE_CTRL = 0,
    USB_EP_TYPE_ISO  = 1,
    USB_EP_TYPE_BULK = 2,
    USB_EP_TYPE_INT  = 3
} usb_ep_type_t;

typedef struct usb_hal_handle usb_hal_handle_t;

usb_hal_handle_t *usb_hal_create(void *peripheral);
void usb_hal_destroy(usb_hal_handle_t *h);
void usb_hal_enable_clock(usb_hal_handle_t *h);
void usb_hal_delay_ms(uint32_t ms);

/* Full device-mode core bring-up: force device mode, embedded FS PHY, soft
 * reset, FIFO sizing, interrupt masks, connect pull-up. Returns 0. */
int  usb_hal_core_init(usb_hal_handle_t *h, int vbus_sense);

/* Activate one endpoint (direction, max packet size, type). */
int  usb_hal_ep_config(usb_hal_handle_t *h, uint8_t ep, int dir_in,
                       uint16_t mps, usb_ep_type_t type);

/* Prime an IN transfer: load `len` bytes into the TX FIFO and enable the EP. */
int  usb_hal_ep_tx(usb_hal_handle_t *h, uint8_t ep, const void *buf, uint16_t len);

/* Prime an OUT receive of up to `len` bytes. */
int  usb_hal_ep_rx(usb_hal_handle_t *h, uint8_t ep, uint16_t len);

/* Read `len` bytes already popped from the RxFIFO (call after RXFLVL). */
void usb_hal_fifo_read(usb_hal_handle_t *h, void *buf, uint16_t len);

/* EP0 status-stage helpers. */
void usb_hal_ep0_tx_zlp(usb_hal_handle_t *h);
void usb_hal_ep0_rx_zlp(usb_hal_handle_t *h);

/* Interrupt status access (GINTSTS is write-1-to-clear for level/mask bits). */
uint32_t usb_hal_gintsts(usb_hal_handle_t *h);
uint32_t usb_hal_gintsts_raw(usb_hal_handle_t *h);   /* unmasked raw GINTSTS */
uint32_t usb_hal_gccfg(usb_hal_handle_t *h);          /* GCCFG (PHY power/sense) */
void     usb_hal_gint_clear(usb_hal_handle_t *h, uint32_t mask);
uint32_t usb_hal_daint(usb_hal_handle_t *h);

/* Pop and return one GRXSTSP word (RxFIFO status). */
uint32_t usb_hal_rxstsp(usb_hal_handle_t *h);

uint32_t usb_hal_doepint(usb_hal_handle_t *h, uint8_t ep);
uint32_t usb_hal_diepint(usb_hal_handle_t *h, uint8_t ep);
void     usb_hal_doepint_clear(usb_hal_handle_t *h, uint8_t ep, uint32_t mask);
void     usb_hal_diepint_clear(usb_hal_handle_t *h, uint8_t ep, uint32_t mask);

void usb_hal_connect(usb_hal_handle_t *h);
void usb_hal_disconnect(usb_hal_handle_t *h);
void usb_hal_set_address(usb_hal_handle_t *h, uint8_t addr);
uint32_t usb_hal_dsts(usb_hal_handle_t *h);

irq_id_t usb_hal_irq_id(usb_hal_handle_t *h);

/* GINTSTS bit positions reused from the CMSIS header. */
#define USB_HAL_GINT_RXFLVL   (1UL << 4)    /* receive FIFO non-empty (level)   */
#define USB_HAL_GINT_SOF      (1UL << 3)
#define USB_HAL_GINT_USBSUSP  (1UL << 11)
#define USB_HAL_GINT_USBRST   (1UL << 12)   /* USB reset                        */
#define USB_HAL_GINT_ENUMDNE  (1UL << 13)   /* enumeration done                 */
#define USB_HAL_GINT_WKUP     (1UL << 15)
#define USB_HAL_GINT_IEPINT   (1UL << 18)   /* IN endpoint interrupt            */
#define USB_HAL_GINT_OEPINT   (1UL << 19)   /* OUT endpoint interrupt           */

#endif /* USB_HAL_H */
