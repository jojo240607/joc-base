#ifndef JOC_RTOS_ARCH_CORTEX_M_MEMMAP_H
#define JOC_RTOS_ARCH_CORTEX_M_MEMMAP_H

#include <stdint.h>

/* ===========================================================================
 * arch/cortex_m 内存映射（芯片相关，换芯片时只改这一处头文件）
 *
 * MPU 是 ISA 特性（所有 Cortex-M 共有），但“区域布局”（Flash / SRAM / 外设的
 * 基址与大小，以及 BIST 备用扇区）属于芯片内存映射，是换芯片时需要调整的
 * 【移植旋钮】。抽出本文件后，mpu.c 不再出现任何具体基址/大小常数，只需按新
 * 芯片重写这里即可。
 *
 * 注意：0x08000000/0x20000000/0x40000000 是 ARMv7-M 架构约定的（所有
 * Cortex-M 通用）代码/SRAM/外设基址，通常不会变；真正随芯片变化的是各区域的
 * 大小，以及 BIST 备用扇区的位置。
 *
 * 区域大小以 log2 表达：MPU RASR.SIZE 字段要求 SIZE = log2(bytes) - 1，
 * 这里直接给出 log2(bytes)，mpu.c 再减 1 即可。
 * AP 见 Cortex-M RASR AP[2:0]；XN=1 禁止在该区域取指。
 * ======================================================================== */

/* --- Region 0: Flash（代码区，只读 + 可执行，保护代码不被改写） --- */
#define MEMMAP_FLASH_BASE       0x08000000u
#define MEMMAP_FLASH_SIZE_LOG2  20u        /* 1 MB */
#define MEMMAP_FLASH_AP         0b110u     /* AP[2:0] = RO（双方） */
#define MEMMAP_FLASH_XN         0          /* 可执行 */

/* --- Region 1: SRAM（可读写，不可执行） --- */
#define MEMMAP_SRAM_BASE        0x20000000u
#define MEMMAP_SRAM_SIZE_LOG2   17u        /* 128 KB */
#define MEMMAP_SRAM_AP          0b011u     /* AP[2:0] = RW（双方） */
#define MEMMAP_SRAM_XN          1          /* 不可执行 */

/* --- Region 2: 外设（仅特权可读写，不可执行） --- */
#define MEMMAP_PERIPH_BASE      0x40000000u
#define MEMMAP_PERIPH_SIZE_LOG2 29u        /* 512 MB */
#define MEMMAP_PERIPH_AP        0b001u     /* AP[2:0] = 仅特权 RW */
#define MEMMAP_PERIPH_XN        1          /* 不可执行 */

/* --- Region 3: Flash BIST 备用扇区（仅特权 RW，不可执行） ---
 * 编号高于 Region0，重叠时高编号优先，使 flash 烧录自检的“写闪存”不被 RO 拦截，
 * 其余 Flash 仍为只读（代码保护）。位置随芯片而异（此处为 STM32F4 的 sector 7）。 */
#define MEMMAP_FLASH_BIST_BASE       0x08060000u
#define MEMMAP_FLASH_BIST_SIZE_LOG2  17u  /* 128 KB */
#define MEMMAP_FLASH_BIST_AP         0b001u /* AP[2:0] = 仅特权 RW */
#define MEMMAP_FLASH_BIST_XN         1     /* 不可执行 */

#endif /* JOC_RTOS_ARCH_CORTEX_M_MEMMAP_H */
