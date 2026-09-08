#ifndef SD_CARD_H
#define SD_CARD_H

#include "iface/block_device.h"
#include "iface/device.h"
#include <stdint.h>

/* ioctl 命令面（SDK ioctl.rs 同步镜像） */
#define SD_CARD_IOCTL_INIT        0x60   /* arg: none — SD 卡初始化序列 */
#define SD_CARD_IOCTL_READ_BLOCK  0x61   /* arg: *const sd_block_io_t — 读扇区 */
#define SD_CARD_IOCTL_WRITE_BLOCK 0x62   /* arg: *const sd_block_io_t — 写扇区 */

/* 块读写参数（与 SDK ioctl.rs 的 SdBlockIo 布局一致，repr(C)） */
typedef struct {
    uint64_t lba;     /* 起始扇区（512B 块） */
    uint32_t count;   /* 扇区数（1 = 单块 CMD17/24） */
    uint8_t *buf;     /* 数据缓冲（count*512 字节） */
} sd_block_io_t;

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
