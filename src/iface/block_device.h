#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include "iface/device.h"
#include <stdint.h>
#include <stddef.h>

/*
 * Block device base class — a subclass of `device`.
 *
 * Covers Flash, NOR/NAND, EEPROM, SD/eMMC, SSD: peripherals organized as a
 * sequence of fixed-size blocks (sectors). The base `device` gives
 * open/close/ioctl; this class adds block-oriented operations.
 *
 * INHERITANCE (C style): embed `device parent` as the FIRST member.
 * Downcast: check `device.class == DEVICE_CLASS_BLOCK`, then cast.
 */
typedef struct _block_device block_device;

typedef struct {
    uint64_t total_bytes;   /* total capacity in bytes */
    uint32_t block_size;    /* block/sector size in bytes */
    uint32_t block_count;   /* number of blocks */
} block_device_info_t;

struct block_deviceVtable {
    /* Read one or more consecutive blocks into buf. Returns 0 on success. */
    int (*read)(block_device *self, uint64_t lba, void *buf, uint32_t count);
    /* Write one or more consecutive blocks from buf. Returns 0 on success. */
    int (*write)(block_device *self, uint64_t lba, const void *buf, uint32_t count);
    /* Erase one or more consecutive blocks (optional; returns -1 if N/A). */
    int (*erase)(block_device *self, uint64_t lba, uint32_t count);
    /* Read device info (capacity, block size, etc.). Returns 0 on success. */
    int (*get_info)(block_device *self, block_device_info_t *info);
};

struct _block_device {
    device parent;                         /* IS-A device */
    const struct block_deviceVtable *vtable;
};

/* upcast/downcast helpers */
static inline device *block_device_to_device(block_device *b) { return &b->parent; }
static inline block_device *device_as_block(device *d)
    { return (d && d->class == DEVICE_CLASS_BLOCK) ? (block_device *)d : NULL; }

#endif /* BLOCK_DEVICE_H */
