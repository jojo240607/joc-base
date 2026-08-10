/*
 * ccm_bss.h — 把"纯软件、不含 DMA 目标缓冲"的 RTOS/驱动静态对象搬入 CCM 的便捷宏。
 *
 * CCM(@0x10000000, 64KB) 仅 CPU 可访问，DMA 访问不到。因此任何含 DMA 目标缓冲
 * (UART/ADC/DAC/SPI/I2S/SDIO 的 dma_bounce、DMA 描述符) 的对象【绝不能】用本宏。
 *
 * 段名由构建注入(RTOS_CCM_DATA_SECTION)：
 *   - 发布版(.ccm_bss)  : 搬入 CCM，收缩主 SRAM .bss，让出的空间下推给 APP_RAM
 *   - 开发版(.bss)      : 留主 SRAM（开发版 CCM 已被自测任务栈占满，不能再挤）
 *
 * 用法：在文件作用域 include 本头，给目标全局变量加 RTOS_CCM_BSS 属性，例如
 *   static int RTOS_CCM_BSS g_my_table[SIZE];
 */
#ifndef JOC_CCM_BSS_H
#define JOC_CCM_BSS_H

#ifndef RTOS_CCM_DATA_SECTION
#define RTOS_CCM_DATA_SECTION ".bss"
#endif

#define RTOS_CCM_BSS __attribute__((section(RTOS_CCM_DATA_SECTION)))

#endif /* JOC_CCM_BSS_H */
