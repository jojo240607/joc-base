#ifndef USB_HAL_H
#define USB_HAL_H

#include <stdint.h>
#include <stddef.h>
#include "usb_core.h"     /* ST: USB_OTG_CORE_HANDLE, USB_OTG_CORE_ID_TypeDef */
#include "usb_dcd.h"      /* ST: DCD_* device-layer API */
#include "usb_dcd_int.h"  /* ST: USBD_OTG_ISR_Handler, USBD_DCD_INT_fops */
#include "usbd_core.h"    /* ST: USBD_Init, USBD_Class_cb_TypeDef, USBD_Usr_cb_TypeDef */
#include "usbd_ioreq.h"   /* ST: USBD_Ctl* control IO helpers */
#include "usbd_req.h"     /* ST: USBD_StdDevReq, USBD_ParseSetupRequest */
#include "usbd_cdc_core.h"/* our CDC class + accessors */
#include "irq_hal.h"      /* irq_id_t */

/*
 * Hardware Abstraction Layer — STM32F4 OTG FS, device mode.
 *
 * This layer WRAPS ST's verified OTG FS silicon + standard-request engine
 * (the STM32_USB_OTG_Driver + USB_Device_Library copied into st_usb/). The
 * driver (drv/usb.c) owns the CDC protocol and the OOP device interface; this
 * HAL only holds the ST core handle and exposes a thin, project-friendly API
 * (create/clock/connect/irq + register-level readbacks for diagnostics).
 *
 * Endpoint usage for the CDC function:
 *   EP0  IN+OUT  control (enumerate)
 *   EP1  IN+OUT  CDC data bulk (the VCP byte stream)
 *   EP2  IN      CDC ACM notification (interrupt)
 */

typedef struct usb_hal_handle {
    USB_OTG_CORE_HANDLE *pdev;
} usb_hal_handle_t;

usb_hal_handle_t *usb_hal_create(void *peripheral);
void usb_hal_destroy(usb_hal_handle_t *h);
USB_OTG_CORE_HANDLE *usb_hal_pdev(usb_hal_handle_t *h);

void usb_hal_enable_clock(usb_hal_handle_t *h);
void usb_hal_connect(usb_hal_handle_t *h);
void usb_hal_disconnect(usb_hal_handle_t *h);
irq_id_t usb_hal_irq_id(usb_hal_handle_t *h);

/* ---- diagnostic register readbacks (for USBSTAT) ---------------------- */
uint32_t usb_hal_gintsts_raw(usb_hal_handle_t *h);
uint32_t usb_hal_gccfg(usb_hal_handle_t *h);
uint32_t usb_hal_dcfg(usb_hal_handle_t *h);
uint32_t usb_hal_dsts(usb_hal_handle_t *h);
uint32_t usb_hal_dctl(usb_hal_handle_t *h);
uint32_t usb_hal_gusbcfg(usb_hal_handle_t *h);
uint32_t usb_hal_diepctl(usb_hal_handle_t *h, uint8_t ep);
uint32_t usb_hal_doepctl(usb_hal_handle_t *h, uint8_t ep);

#endif /* USB_HAL_H */
