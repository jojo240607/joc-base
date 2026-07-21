#ifndef SD_CARD_H
#define SD_CARD_H

#include "iface/block_device.h"
#include "iface/device.h"
#include <stdint.h>

/*
 * SD Card driver — a BLOCK device that implements the SD protocol.
 *
 * BUS ABSTRACTION: sd_card communicates with the SD card through an
 * underlying bus device (SDIO or SPI). The bus device is looked up by
 * name via device_manager and driven through its ioctl interface.
 * This design allows:
 *   - SDIO mode: bus_dev = "sdio0", commands via SDIO_IOCTL_CMD
 *   - SPI mode  : bus_dev = "spi0", commands via SPI_IOCTL_XFER
 *     (SPI mode implementation to be added when needed)
 */

/* Bus operations — abstracts away the underlying transport.
 * For SDIO: send_cmd → ioctl(dev, SDIO_IOCTL_CMD, &sdio_cmd_t)
 * For SPI:  send_cmd → ioctl(dev, SPI_IOCTL_XFER, &spi_xfer_t) (future) */
typedef struct {
    const char *name;        /* logical device name */
    const char *bus_name;    /* device_manager name of the bus device */
    int bus_type;            /* 0=SDIO, 1=SPI (reserved for future) */
} sd_card_config_t;

typedef struct _sd_card sd_card;

struct _sd_card {
    block_device parent;
    device *bus_dev;              /* the underlying bus device */
    int      bus_type;            /* 0=SDIO, 1=SPI */
    uint16_t rca;
    uint8_t  csd[16], cid[16];
    uint32_t block_len;
    uint32_t card_size;           /* in 512-byte blocks */
    int      card_type;           /* 0=none,1=SDSC,2=SDHC */
    int      ready;
};

device *sd_card_create(const void *config);
void sd_card_destroy(sd_card *self);

#endif /* SD_CARD_H */
