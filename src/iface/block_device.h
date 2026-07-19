#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include "iface/device.h"
#include <stddef.h>

/*
 * 块设备 (Block) base class — a subclass of `device`.
 *
 * Covers on-chip Flash, external NOR/NAND Flash, EEPROM, SD card, eMMC:
 * addressable storage that is erased before write and addressed by block.
 * The base `device` gives open/close/ioctl; this class adds block-addressed
 * ops (read_block / write_block / erase_block / get_geometry) instead of the
 * byte-stream read/write, which do not fit a Flash.
 */

typedef struct _block_device block_device;

struct block_deviceVtable {
    int (*read_block)(block_device *self, uint32_t block, void *buf, size_t bcount);
    int (*write_block)(block_device *self, uint32_t block, const void *buf, size_t bcount);
    int (*erase_block)(block_device *self, uint32_t block, size_t bcount);
    int (*get_geometry)(block_device *self, uint32_t *block_size, uint32_t *block_count);
};

struct _block_device {
    device parent;
    const struct block_deviceVtable *vtable;
};

static inline device *block_device_to_device(block_device *b) { return &b->parent; }
static inline block_device *device_as_block(device *d)
    { return (d && d->class == DEVICE_CLASS_BLOCK) ? (block_device *)d : NULL; }

#endif /* BLOCK_DEVICE_H */
