#ifndef USB_H
#define USB_H

#include "iface/device.h"
#include "iface/stream_device.h"

/*
 * USB CDC-ACM (Virtual COM Port) driver — STM32F4 OTG FS, device mode.
 *
 * Implements the standard CDC-ACM function (IAD + Communication + Data
 * interfaces) so a PC sees a plain serial port (no INF on Windows thanks to
 * the Miscellaneous-class + IAD descriptors). Mounts on the STREAM device
 * class: write()/read() move the VCP byte stream, EP1 IN/OUT carry the bulk
 * data, EP0 carries enumeration, EP2 IN carries the ACM notification.
 *
 * Full enumeration requires a USB host on the Discovery's CN5 connector; the
 * BIST therefore validates (a) the OTG FS core comes up (clock/PHY/FIFO/EPs)
 * and (b) the control-protocol logic via a self-contained, host-free test that
 * feeds synthetic SETUP packets and checks the produced responses.
 */

typedef struct {
    const char *name;
    void       *periph;      /* USB_OTG_FS (ignored; HAL uses the fixed base) */
    const char *dm_signal;   /* e.g. "USB_OTG_FS_DM"  (PA11, AF10) */
    const char *dp_signal;   /* e.g. "USB_OTG_FS_DP"  (PA12, AF10) */
    int         vbus_sense;  /* 1 = use VBUS sensing, 0 = ignore (NOVBUSSENS) */
} usb_config_t;

/* ioctl commands (driver-specific, above the shared STREAM ioctls). */
#define USB_IOCTL_GET_GINTSTS      0xD0
#define USB_IOCTL_GET_GCCFG        0xD1
#define USB_IOCTL_GET_DSTS         0xD2
#define USB_IOCTL_GET_ADDRESS      0xD3
#define USB_IOCTL_CONNECTED        0xD4
#define USB_IOCTL_SET_LINE_CODING  0xD5   /* arg: uint8_t[7] */
#define USB_IOCTL_GET_LINE_CODING  0xD6   /* arg: uint8_t[7] */
#define USB_IOCTL_RUN_CTRL_SELFTEST 0xD7  /* arg: NULL; ret 0=pass, -1=fail */
#define USB_IOCTL_DBG_DUMP         0xD8  /* arg: NULL; print ISR/enum counters */
#define USB_IOCTL_DBG_SET          0xD9  /* arg: int* (0/1); toggle ISR trace */
#define USB_IOCTL_SET_DAD_TEST     0xDA  /* arg: uint8_t* (addr); write DAD, print readback */

#define USB_RX_BUF_SIZE  256

typedef struct _usb usb;

device *usb_create(const void *config);
void     usb_destroy(usb *self);

#endif /* USB_H */
