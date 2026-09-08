#ifndef FSMC_H
#define FSMC_H

#include "iface/device.h"
#include "hal/stm32/fsmc_hal.h"
#include <stdint.h>

/*
 * FSMC（灵活静态存储器控制器）驱动 — F407 @0xA0000000。
 *
 * 验证面：BCR/BTR/BWTR 寄存器配置读写往返 + Bank1 片选窗口（0x60000000）
 * 32 位读写往返。open 只开时钟（AHB3 FSMCEN），BCR 配置与片选使能经 ioctl；
 * read/write 面直通 Bank1 窗口（未使能窗口访问返回 -2，防误写）。
 */

/* device-level ioctl commands（SDK ioctl.rs 同步镜像） */
#define FSMC_IOCTL_SET_BCR   0x80   /* arg: *const u32 BCR1 值 */
#define FSMC_IOCTL_GET_BCR   0x81   /* arg: *mut u32 BCR1 回读 */
#define FSMC_IOCTL_SET_BTR   0x82   /* arg: *const u32 BTR1 值 */
#define FSMC_IOCTL_GET_BTR   0x83   /* arg: *mut u32 BTR1 回读 */
#define FSMC_IOCTL_GET_BWTR  0x84   /* arg: *mut u32 BWTR1 回读 */
#define FSMC_IOCTL_BANK1_ENABLE 0x85 /* arg: none — BCR1.MBKEN=1（窗口可用） */

/* BCR 位（F407 FSMC_BCR1，CMSIS 宏同义） */
#define FSMC_BCR_MBKEN  0x00000001UL   /* memory bank enable */
#define FSMC_BCR_WREN   0x00001000UL   /* write enable */
#define FSMC_BCR_MTYP   0x0000000CUL   /* memory type[1:0] */
#define FSMC_BCR_MWID   0x00000030UL   /* memory data bus width[1:0] */
#define FSMC_BCR_EXTMOD 0x00004000UL   /* extended mode enable */

typedef struct _fsmc fsmc;

struct _fsmc {
    device parent;              /* unified interface — MUST be first member */
    fsmc_hal_handle_t *hal;
};

device *fsmc_create(const void *config);
void fsmc_destroy(fsmc *self);

/* Driver-specific board config — defined HERE, filled by the board. */
typedef struct {
    const char *name;    /* logical device name (e.g. "fsmc0") */
    void *periph;        /* FSMC register base (e.g. (void *)FSMC_R_BASE) */
} fsmc_config_t;

#endif /* FSMC_H */
