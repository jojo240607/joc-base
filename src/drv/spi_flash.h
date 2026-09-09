#ifndef SPI_FLASH_H
#define SPI_FLASH_H

#include "iface/device.h"
#include <stdint.h>

/* device-level control commands for the SPI NOR Flash (W25Q 类) driver */
#define FLASH_IOCTL_GET_JEDEC     0x01 /* arg: uint32_t*  — 3B JEDEC ID（大端 u24，如 0xEF4018） */
#define FLASH_IOCTL_READ          0x02 /* arg: flash_io_t* — 读 len 字节 */
#define FLASH_IOCTL_WRITE         0x03 /* arg: flash_io_t* — 页编程写 len 字节（≤256，页内回绕） */
#define FLASH_IOCTL_ERASE_SECTOR  0x04 /* arg: uint32_t*  — 扇区擦除（4KB，地址对齐） */

/* 读/写参数块 */
typedef struct {
    uint32_t addr;   /* 24bit 偏移 */
    uint16_t len;    /* 字节数 */
    uint8_t *buf;    /* 数据指针 */
} flash_io_t;

/*
 * SPI NOR Flash（W25Q128 类，用于保存/存储）。
 *
 * SPI 命令流（CS 低开始、高结束）：
 *   JEDEC ID : 0x9F → 3B（厂商 0xEF / 容量 0x40 / 型号 0x18）
 *   READ     : 0x03 + 3B 地址 + N 字节数据（地址自增）
 *   PAGE_PROG: WREN(0x06) → 0x02 + 3B 地址 + N 字节数据（≤256B，页内回绕）
 *   SECTOR_ERASE: WREN → 0x20 + 3B 地址（4KB 对齐）
 * 写/擦除前必须 WREN（模拟器 WEL 锁存；CS 上升沿提交并清 WEL）。
 *
 * 实现统一 device 接口：open 做 JEDEC ID 校验（失败即驱动层拒绝），
 * ioctl 提供 GET_JEDEC / READ / WRITE / ERASE_SECTOR。
 */
typedef struct _spi_flash spi_flash;

/* 板级数据：设备名 + 依赖的 SPI 总线与片选 GPIO（device 名） */
typedef struct {
    const char *name; /* e.g. "spi_flash0" */
    const char *spi;  /* e.g. "spi1"（SPI2 @ PI1/2/3） */
    const char *cs;   /* e.g. "spi_flash_cs"（GPIO 输出，默认高=不选中） */
} spi_flash_config_t;

/* 工厂：读 config、解析依赖设备名（create 时解析并拉起依赖 + JEDEC 校验） */
device *spi_flash_create(const void *config);

#endif /* SPI_FLASH_H */
