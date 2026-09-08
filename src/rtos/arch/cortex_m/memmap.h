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
#define MEMMAP_FLASH_TEX        0u
#define MEMMAP_FLASH_C          0u
#define MEMMAP_FLASH_B          0u

/* --- Region 1: SRAM（可读写，不可执行） --- */
#define MEMMAP_SRAM_BASE        0x20000000u
#define MEMMAP_SRAM_SIZE_LOG2   17u        /* 128 KB */
#define MEMMAP_SRAM_AP          0b011u     /* AP[2:0] = RW（双方） */
#define MEMMAP_SRAM_XN          1          /* 不可执行 */
#define MEMMAP_SRAM_TEX         0u
#define MEMMAP_SRAM_C           0u
#define MEMMAP_SRAM_B           0u

/* --- Region 2: 外设（仅特权可读写，不可执行） --- */
#define MEMMAP_PERIPH_BASE      0x40000000u
#define MEMMAP_PERIPH_SIZE_LOG2 29u        /* 512 MB */
#define MEMMAP_PERIPH_AP        0b001u     /* AP[2:0] = 仅特权 RW */
#define MEMMAP_PERIPH_XN        1          /* 不可执行 */
#define MEMMAP_PERIPH_TEX       0u
#define MEMMAP_PERIPH_C         0u
#define MEMMAP_PERIPH_B         0u

/* --- Region 3: Flash BIST 备用扇区（仅特权 RW，不可执行） ---
 * 编号高于 Region0，重叠时高编号优先，使 flash 烧录自检的"写闪存"不被 RO 拦截，
 * 其余 Flash 仍为只读（代码保护）。位置随芯片而异。
 *
 * 注意：阶段 2 起 APP_FLASH 应用分区占用 sector 7/8/9（0x08060000 起 384KB），
 * 故 BIST 备用扇区必须移到【应用分区之上】的 sector 11（0x080E0000），否则 Region3
 * 的 XN=1 会覆盖应用分区导致 App 取指触发 IACCVIOL。应用分区只受 Region0（全 Flash
 * RO+可执行）覆盖，得以正常执行。 */
#define MEMMAP_FLASH_BIST_BASE       0x080E0000u
#define MEMMAP_FLASH_BIST_SIZE_LOG2  17u  /* 128 KB (sector 11) */
#define MEMMAP_FLASH_BIST_AP         0b001u /* AP[2:0] = 仅特权 RW */
#define MEMMAP_FLASH_BIST_XN         1     /* 不可执行 */
#define MEMMAP_FLASH_BIST_TEX        0u
#define MEMMAP_FLASH_BIST_C          0u
#define MEMMAP_FLASH_BIST_B          0u

/* --- Region 5: CCM（内核对象区，0x10000000, 64K，CPU 专用、DMA 不可达） ---
 * CCM 存放 TCB 池(g_task_pool) 与所有任务栈（见 docs/rtos-design.md §4.5：为腾出
 * 主 SRAM 给 DMA 缓冲而搬入）。非特权任务在"经 SVC 门前"就要读 g_running(TCB) 判断
 * 是否需陷门（rtos_need_svc）、且必须读写自身栈，故对 CCM 开放 unpriv RW(XN)。
 * 与 R4 每任务栈 region 重叠处属性一致(均 unpriv RW+XN)，高编号优先但无冲突；CCM
 * 不可被 DMA 访问，其数据天然与 DMA 缓冲隔离，开放 unpriv 访问不引入 DMA 越权风险。 */
#define MEMMAP_CCM_BASE              0x10000000u
#define MEMMAP_CCM_SIZE_LOG2         16u  /* 64 KB */
#define MEMMAP_CCM_AP                0b011u /* AP[2:0] = RW（双方） */
#define MEMMAP_CCM_XN                1     /* 不可执行 */
#define MEMMAP_CCM_TEX               0u
#define MEMMAP_CCM_C                 0u
#define MEMMAP_CCM_B                 0u

/* ========================================================================
 * STM32H750VB 扩展区域（AXI SRAM、QSPI、SRAM1、DTCM 替代 CCM）
 * ====================================================================== */
#ifdef STM32H750xx
/* H750 Flash 仅 128KB —— 覆盖 F4 默认（1MB）并缩小 SIZE。 */
#undef  MEMMAP_FLASH_SIZE_LOG2
#define MEMMAP_FLASH_SIZE_LOG2  17u        /* 128 KB */
#undef  MEMMAP_FLASH_TEX
#undef  MEMMAP_FLASH_C
#undef  MEMMAP_FLASH_B
#define MEMMAP_FLASH_TEX        0u         /* Normal WT（代码区可缓存） */
#define MEMMAP_FLASH_C          1u
#define MEMMAP_FLASH_B          0u

/* H750: 0x20000000 是 DTCM（128KB），替代 F4 的主 SRAM。DTCM 绕过 D-Cache。 */
#undef  MEMMAP_SRAM_BASE
#undef  MEMMAP_SRAM_SIZE_LOG2
#undef  MEMMAP_SRAM_TEX
#undef  MEMMAP_SRAM_C
#undef  MEMMAP_SRAM_B
#define MEMMAP_SRAM_BASE        0x20000000u  /* DTCM */
#define MEMMAP_SRAM_SIZE_LOG2   17u          /* 128 KB */
#define MEMMAP_SRAM_TEX         0u           /* Normal WB+WA（TCM 忽略缓存属性） */
#define MEMMAP_SRAM_C           1u
#define MEMMAP_SRAM_B           1u

/* H750 外设总线放款到 1GB（0x40000000..0x7FFFFFFF），覆盖 AHB1/2/3 + APB1/2/3/4 */
#undef  MEMMAP_PERIPH_SIZE_LOG2
#undef  MEMMAP_PERIPH_TEX
#undef  MEMMAP_PERIPH_C
#undef  MEMMAP_PERIPH_B
#define MEMMAP_PERIPH_SIZE_LOG2 30u         /* 1 GB */
#define MEMMAP_PERIPH_TEX       0u          /* Device nGnRE */
#define MEMMAP_PERIPH_C         0u
#define MEMMAP_PERIPH_B         1u

/* H750 无 BIST 备用扇区（Flash 仅 128KB，无空闲扇区）。Region3 留空（SIZE=0 关闭）。 */

/* H750 无独立 CCM：DTCM 既是主数据区也是"内核专用"，R5 以相同属性覆盖 DTCM。 */
#undef  MEMMAP_CCM_BASE
#undef  MEMMAP_CCM_SIZE_LOG2
#undef  MEMMAP_CCM_TEX
#undef  MEMMAP_CCM_C
#undef  MEMMAP_CCM_B
#define MEMMAP_CCM_BASE         0x20000000u  /* DTCM（替代 CCM 作为内核对象区） */
#define MEMMAP_CCM_SIZE_LOG2    17u          /* 128 KB */
#define MEMMAP_CCM_TEX          0u
#define MEMMAP_CCM_C            1u
#define MEMMAP_CCM_B            1u

/* --- Region 6: AXI SRAM（0x24000000, 512KB, 可缓存数据区） --- */
#define MEMMAP_AXI_BASE         0x24000000u
#define MEMMAP_AXI_SIZE_LOG2    19u          /* 512 KB */
#define MEMMAP_AXI_AP           0b011u       /* RW 双方 */
#define MEMMAP_AXI_XN           1u           /* 不可执行 */
#define MEMMAP_AXI_TEX          0u           /* Normal WB+WA */
#define MEMMAP_AXI_C            1u
#define MEMMAP_AXI_B            1u

/* --- Region 7: QSPI XIP（0x90000000, 16MB, 只读可执行） --- */
#define MEMMAP_QSPI_BASE        0x90000000u
#define MEMMAP_QSPI_SIZE_LOG2   24u          /* 16 MB */
#define MEMMAP_QSPI_AP          0b110u       /* RO 双方 */
#define MEMMAP_QSPI_XN          0u           /* 可执行 */
#define MEMMAP_QSPI_TEX         0u           /* Normal WT（XIP 代码只读不写） */
#define MEMMAP_QSPI_C           1u
#define MEMMAP_QSPI_B           0u

/* --- Region 8: SRAM1（0x30000000, 128KB, 非缓存 → DMA 一致） --- */
#define MEMMAP_SRAM1_BASE       0x30000000u
#define MEMMAP_SRAM1_SIZE_LOG2  17u          /* 128 KB */
#define MEMMAP_SRAM1_AP         0b011u       /* RW 双方 */
#define MEMMAP_SRAM1_XN         1u           /* 不可执行 */
#define MEMMAP_SRAM1_TEX        1u           /* Normal non-cacheable（外设: TEX=1,C=0,B=0） */
#define MEMMAP_SRAM1_C          0u
#define MEMMAP_SRAM1_B          0u
#endif /* STM32H750xx */

#endif /* JOC_RTOS_ARCH_CORTEX_M_MEMMAP_H */
