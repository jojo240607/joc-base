/**
 * usbd_cdc_core.h — CDC-ACM (Virtual COM Port) class driver for the ST USB
 * device library, adapted to the joc-base OOP framework.
 *
 * This is the ONLY project-specific piece of the USB stack: it implements the
 * USBD_Class_cb_TypeDef contract (endpoint open/close, control-class requests,
 * bulk IN/OUT data) and reaches the OOP `usb` driver's ring buffer and
 * line-coding state through the accessor functions declared here (defined in
 * usb.c). The silicon + standard-request engine (ST's usb_core/usb_dcd/usbd_*
 * files) is used unmodified.
 */
#ifndef __USB_CDC_CORE_H_
#define __USB_CDC_CORE_H_

#include "usbd_ioreq.h"

struct _usb;   /* forward declaration of the OOP driver object */

/* Accessors implemented in usb.c; the CDC class uses them to reach the
 * driver's ring buffer and line-coding state without knowing the struct. */
void     usbd_cdc_register_usb(struct _usb *u);
uint8_t *usbd_cdc_line_coding_ptr(void);
void     usbd_cdc_on_line_state(uint8_t s);
void     usbd_cdc_rx_push(const uint8_t *data, uint16_t len);
void     usbd_cdc_tx_done(void);
void     usb_cdc_apply_line_coding(const uint8_t *buf, uint16_t len);

/* CDC class request codes */
#define SET_LINE_CODING         0x20
#define GET_LINE_CODING         0x21
#define SET_CONTROL_LINE_STATE  0x22

extern USBD_Class_cb_TypeDef USBD_CDC_cb;
extern uint8_t cdc_config_descriptor[75];

/* Exposed for the host-free control self-test in usb.c. */
uint8_t cdc_Setup(void *pdev, USB_SETUP_REQ *req);
uint8_t cdc_EP0_RxReady(void *pdev);
/* Copy host OUT-data (e.g. SET_LINE_CODING payload) into the EP0 rx buffer and
 * run the RxReady callback, emulating the data stage without a real host. */
void    usbd_cdc_feed_cmd(const uint8_t *buf, uint16_t len);

#endif /* __USB_CDC_CORE_H_ */
