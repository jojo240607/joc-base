#ifndef FSMC_HAL_H
#define FSMC_HAL_H

#include <stdint.h>
#include <stddef.h>

/*
 * Hardware Abstraction Layer — STM32F4 FSMC (flexible static memory
 * controller, F407 @0xA0000000).
 *
 * The ONLY place that touches the FSMC registers. The driver layer stays
 * register-free and only calls these functions. The F407 FSMC needs no
 * external pins for the register/CS-window surface we exercise; the memory
 * bus itself is implicit (NOR/SRAM chip-select windows Bank1..4).
 *
 * 寄存器面：BCR1-4/BTR1-4（FSMC_Bank1_TypeDef，0x00-0x1C）+ BWTR1-4
 * （FSMC_Bank1E_TypeDef，0x104-0x110）。BCRn.MBKEN（bit0）使能对应片选窗口；
 * Bank1 窗口映射 @0x60000000（模拟器：64KB 后备缓冲；真机：外部存储器总线）。
 */

typedef struct fsmc_hal_handle fsmc_hal_handle_t;

fsmc_hal_handle_t *fsmc_hal_create(void *peripheral);
void fsmc_hal_destroy(fsmc_hal_handle_t *h);

/* Gate the FSMC clock (RCC AHB3ENR.FSMCEN). Idempotent. */
void fsmc_hal_enable_clock(fsmc_hal_handle_t *h);

/* BCRn / BTRn / BWTRn 读写（bank = 1..4）。写值按硬件可写位保存、可回读。 */
uint32_t fsmc_hal_get_bcr(fsmc_hal_handle_t *h, int bank);
void     fsmc_hal_set_bcr(fsmc_hal_handle_t *h, int bank, uint32_t val);
uint32_t fsmc_hal_get_btr(fsmc_hal_handle_t *h, int bank);
void     fsmc_hal_set_btr(fsmc_hal_handle_t *h, int bank, uint32_t val);
uint32_t fsmc_hal_get_bwtr(fsmc_hal_handle_t *h, int bank);

/* Bank1 片选使能：BCR1.MBKEN=1（窗口映射生效，后续窗口读写可用）。 */
void fsmc_hal_bank1_enable(fsmc_hal_handle_t *h);
int  fsmc_hal_bank1_enabled(fsmc_hal_handle_t *h);

/* Bank1 片选窗口（0x60000000）32 位读写。off 为窗口内偏移。 */
int  fsmc_hal_bank1_read32(fsmc_hal_handle_t *h, uint32_t off, uint32_t *v);
int  fsmc_hal_bank1_write32(fsmc_hal_handle_t *h, uint32_t off, uint32_t v);

#endif /* FSMC_HAL_H */
