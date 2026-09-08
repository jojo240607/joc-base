/**
 * usb_conf.h — project configuration for the ST USB OTG FS device stack.
 *
 * Adapted from ST's USB_Device_Library template. Defines the core/PHY mode
 * and the OTG FS FIFO sizing. VBUS sensing is DISABLED because the Discovery
 * board does not route VBUS to the OTG_FS VBUS pin (we rely on NOVBUSSENS).
 */
#ifndef __USB_CONF__H__
#define __USB_CONF__H__

#ifdef STM32H750xx
  #include "stm32h750xx.h"
  #include "core_cm7.h"
#else
  #include "stm32f4xx.h"
#endif

/* ---- Core / PHY selection ---------------------------------------------- */
#define USE_USB_OTG_FS
#define USB_OTG_FS_CORE
#define USE_DEVICE_MODE
/* #define VBUS_SENSING_ENABLED  -- left undefined: ignore VBUS sense */

/* ---- Device-library limits --------------------------------------------- */
#define USBD_ITF_MAX_NUM   2      /* Communication IF + Data IF */
#define USBD_CFG_MAX_NUM   1
/* USBD_LPM_ENABLED left undefined -> no BOS descriptor (FS only) */

/* ---- FIFO sizing (in 32-bit words); OTG FS total RAM = 320 words -------
 * RX(128) + TX0/EP0(64) + TX1/EP1 bulk(64) + TX2/EP2 intr(64) = 320.
 * TX3 (EP3) is unused by CDC, so it is left at 0. usb_core.c always
 * programs DIEPTXF[2] with TX3_FIFO_FS_SIZE, hence it must be defined. */
#ifdef USB_OTG_FS_CORE
 #define RX_FIFO_FS_SIZE   128
 #define TX0_FIFO_FS_SIZE  64
 #define TX1_FIFO_FS_SIZE  64
 #define TX2_FIFO_FS_SIZE  64
 #define TX3_FIFO_FS_SIZE  0
#endif

/* ---- Compiler keywords (needed by some ST structs) --------------------- */
#define __ALIGN_BEGIN
#define __ALIGN_END
#if defined (__GNUC__)
 #define __packed __attribute__((__packed__))
#else
 #define __packed
#endif

#endif /* __USB_CONF__H__ */
