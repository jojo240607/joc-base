/**
 * usbd_cdc_core.c — CDC-ACM class driver (see usbd_cdc_core.h).
 *
 * Bulk data path:
 *   - EP1 OUT  (0x01): host -> device. On reception we push the bytes into the
 *     driver ring buffer and re-arm the endpoint for the next packet.
 *   - EP1 IN   (0x81): device -> host. The driver's stream write calls
 *     DCD_EP_Tx directly; the transfer-complete ISR clears bulk_tx_pending.
 *   - EP2 IN   (0x82): CDC ACM notification (interrupt); unused for basic VCP
 *     but opened so the descriptor's notification endpoint is valid.
 */
#include "usbd_cdc_core.h"
#include "usb_dcd.h"
#include "usbd_req.h"

/* CDC-ACM configuration descriptor (IAD + Communication IF + Data IF), 75 B.
 * bDeviceClass is 0xEF (Miscellaneous + IAD) so Windows loads the generic
 * usbser driver without an INF. Kept in writable RAM because usbd_req.c
 * rewrites pbuf[1] to the CONFIGURATION type when answering GET_DESCRIPTOR. */
uint8_t cdc_config_descriptor[75] = {
    0x09, 0x02, 0x4B, 0x00, 0x02, 0x01, 0x00, 0xC0, 0x32, /* config header */
    0x08, 0x0B, 0x00, 0x02, 0x02, 0x02, 0x00, 0x00,       /* IAD */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, /* Comm IF */
    0x05, 0x24, 0x00, 0x10, 0x01,                         /* Header func */
    0x04, 0x24, 0x02, 0x02,                               /* ACM func */
    0x05, 0x24, 0x06, 0x00, 0x01,                         /* Union func */
    0x05, 0x24, 0x01, 0x00, 0x01,                         /* Call Mgmt func */
    0x07, 0x05, 0x82, 0x03, 0x0A, 0x00, 0x10,             /* EP2 IN intr */
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00, /* Data IF */
    0x07, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00,             /* EP1 OUT bulk */
    0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00              /* EP1 IN bulk */
};

static uint8_t  cdc_rx_buf[64];
static uint8_t  cdc_cmd_buf[16];
static uint32_t cdcCmd = 0xFF;
static uint32_t cdcLen = 0;

static uint8_t cdc_Init(void *pdev, uint8_t cfgidx)
{
    (void)cfgidx;
    DCD_EP_Open(pdev, 0x81, 64, USB_OTG_EP_BULK);
    DCD_EP_Open(pdev, 0x01, 64, USB_OTG_EP_BULK);
    DCD_EP_Open(pdev, 0x82, 10, USB_OTG_EP_INT);
    DCD_EP_PrepareRx(pdev, 0x01, cdc_rx_buf, 64);
    return USBD_OK;
}

static uint8_t cdc_DeInit(void *pdev, uint8_t cfgidx)
{
    (void)cfgidx;
    DCD_EP_Close(pdev, 0x81);
    DCD_EP_Close(pdev, 0x01);
    DCD_EP_Close(pdev, 0x82);
    return USBD_OK;
}

uint8_t cdc_Setup(void *pdev, USB_SETUP_REQ *req)
{
    if ((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS)
    {
        if (req->wLength)
        {
            if (req->bmRequest & 0x80)
            {
                /* device-to-host (GET_LINE_CODING) */
                USBD_CtlSendData(pdev, usbd_cdc_line_coding_ptr(), req->wLength);
            }
            else
            {
                /* host-to-device with data stage (SET_LINE_CODING) */
                cdcCmd = req->bRequest;
                cdcLen = req->wLength;
                USBD_CtlPrepareRx(pdev, cdc_cmd_buf, req->wLength);
            }
        }
        else
        {
            /* no-data class request (SET_CONTROL_LINE_STATE) */
            usbd_cdc_on_line_state((uint8_t)(req->wValue & 0x3));
            USBD_CtlSendStatus(pdev);
        }
        return USBD_OK;
    }

    USBD_CtlError(pdev, req);
    return USBD_FAIL;
}

uint8_t cdc_EP0_RxReady(void *pdev)
{
    (void)pdev;
    if (cdcCmd != 0xFF)
    {
        usb_cdc_apply_line_coding(cdc_cmd_buf, (uint16_t)cdcLen);
        cdcCmd = 0xFF;
    }
    return USBD_OK;
}

void usbd_cdc_feed_cmd(const uint8_t *buf, uint16_t len)
{
    uint16_t n = len < (uint16_t)sizeof(cdc_cmd_buf) ? len : (uint16_t)sizeof(cdc_cmd_buf);
    for (uint16_t i = 0; i < n; i++)
        cdc_cmd_buf[i] = buf[i];
    cdc_EP0_RxReady(0);
}

static uint8_t cdc_DataIn(void *pdev, uint8_t epnum)
{
    (void)pdev;
    if (epnum == 1)
        usbd_cdc_tx_done();   /* bulk IN transfer complete -> unblock writer */
    return USBD_OK;
}

static uint8_t cdc_DataOut(void *pdev, uint8_t epnum)
{
    if (epnum == 1)
    {
        uint16_t cnt = ((USB_OTG_CORE_HANDLE *)pdev)->dev.out_ep[1].xfer_count;
        usbd_cdc_rx_push(cdc_rx_buf, cnt);
        DCD_EP_PrepareRx(pdev, 0x01, cdc_rx_buf, 64);
    }
    return USBD_OK;
}

static uint8_t cdc_SOF(void *pdev)
{
    (void)pdev;
    return USBD_OK;
}

static uint8_t *cdc_GetCfgDesc(uint8_t speed, uint16_t *length)
{
    (void)speed;
    *length = sizeof(cdc_config_descriptor);
    return (uint8_t *)cdc_config_descriptor;
}

USBD_Class_cb_TypeDef USBD_CDC_cb = {
    .Init             = cdc_Init,
    .DeInit           = cdc_DeInit,
    .Setup            = cdc_Setup,
    .EP0_TxSent       = NULL,
    .EP0_RxReady      = cdc_EP0_RxReady,
    .DataIn           = cdc_DataIn,
    .DataOut          = cdc_DataOut,
    .SOF              = cdc_SOF,
    .IsoINIncomplete  = NULL,
    .IsoOUTIncomplete = NULL,
    .GetConfigDescriptor = cdc_GetCfgDesc,
};
