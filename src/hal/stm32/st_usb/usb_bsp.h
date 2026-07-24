/**
 * usb_bsp.h — board-support prototypes for the ST USB OTG FS stack.
 * Implemented in usb_bsp.c using our CMSIS register names (no SPL).
 */
#ifndef __USB_BSP__H__
#define __USB_BSP__H__

#include "usb_core.h"

void USB_OTG_BSP_Init (USB_OTG_CORE_HANDLE *pdev);
void USB_OTG_BSP_uDelay (const uint32_t usec);
void USB_OTG_BSP_mDelay (const uint32_t msec);
void USB_OTG_BSP_EnableInterrupt (USB_OTG_CORE_HANDLE *pdev);
void USB_OTG_BSP_TimerIRQ (void);

#endif /* __USB_BSP__H__ */
