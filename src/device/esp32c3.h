#ifndef JOC_DEVICE_ESP32C3_H
#define JOC_DEVICE_ESP32C3_H

/* ===========================================================================
 * ESP32-C3 (RISC-V RV32IMC) 设备头（Phase-1 Renode 仿真）
 *
 * 核心（CLINT/PLIC/CSR/mtvec）定义在 src/rtos/arch/riscv/riscv.h；
 * 本头只描述 ESP32-C3 外设（UART/GPIO）与平台常量。
 *
 * Renode 无 ESP32-C3 外设模型，Phase-1 用 UART.NS16550 模型挂载在
 * ESP32-C3 UART0 的地址 0x60000000 上（uart_hal.c 按 16550 寄存器布局实现，
 * 见 src/hal/esp32c3/uart_hal.c 头注释；换真实芯片时重写该 HAL 即可）。
 * ========================================================================= */

#include <stdint.h>

/* ---- 核心时钟 ---- */
#define ESP32C3_CPU_HZ          40000000u   /* Renode RiscV32 默认 40 MHz */

/* ---- 内存映射 ---- */
#define ESP32C3_IRAM_BASE       0x40380000u /* 192 KB 指令 RAM */
#define ESP32C3_IRAM_SIZE       0x30000u
#define ESP32C3_DRAM_BASE       0x3FC80000u /* 192 KB 数据 RAM */
#define ESP32C3_DRAM_SIZE       0x30000u
#define ESP32C3_FLASH_MMAP_BASE 0x40200000u /* flash memory-mapped（App 分区头） */

/* ---- 外设基址 ---- */
#define ESP32C3_UART0_BASE      0x60000000u /* UART0（Phase-1：NS16550 模型） */
#define ESP32C3_GPIO_BASE       0x60004000u /* GPIO（Phase-1：不模拟，Tag 吸收） */

/* ---- PLIC 外设中断源号（irq id = 16 + 源号，见 riscv.h） ---- */
#define ESP32C3_IRQ_GPIO        1u
#define ESP32C3_IRQ_UART0       13u
#define ESP32C3_IRQ_UART1       14u
#define ESP32C3_IRQ_TIMER0      16u   /* 片上定时器组（机器定时器节拍走 CLINT，id 7） */

/* ---- UART0 寄存器（Phase-1 NS16550 替代布局；DLAB 门控寄存器在 LCR.7=0 时为
 *     数据/中断寄存器，LCR.7=1 时为波特率分频寄存器） ---- */
#define ESP32C3_UART_RBR        0x00u   /* 读：接收缓冲（DLAB=0） */
#define ESP32C3_UART_THR        0x00u   /* 写：发送保持（DLAB=0） */
#define ESP32C3_UART_DLL        0x00u   /* 波特率分频低字节（DLAB=1） */
#define ESP32C3_UART_IER        0x01u   /* 中断使能（DLAB=0） */
#define ESP32C3_UART_DLH        0x01u   /* 波特率分频高字节（DLAB=1） */
#define ESP32C3_UART_FCR        0x02u   /* FIFO 控制 */
#define ESP32C3_UART_IIR        0x02u   /* 中断标识（读） */
#define ESP32C3_UART_LCR        0x03u   /* 线路控制：8N1=0x03, DLAB=0x80 */
#define ESP32C3_UART_MCR        0x04u   /* 调制解调控制 */
#define ESP32C3_UART_LSR        0x05u   /* 线路状态 */
#define ESP32C3_UART_MSR        0x06u   /* 调制解调状态 */
#define ESP32C3_UART_SCR        0x07u   /* 暂存 */

/* 16550 位定义 */
#define ESP32C3_UART_IER_ERBFI  (1u << 0)   /* 接收数据可用中断使能 */
#define ESP32C3_UART_IER_ETBEI  (1u << 1)   /* 发送保持空中断使能 */
#define ESP32C3_UART_FCR_FIFO   (1u << 0)   /* FIFO 使能 */
#define ESP32C3_UART_FCR_RXCLR  (1u << 1)   /* 清 RX FIFO */
#define ESP32C3_UART_FCR_TXCLR  (1u << 2)   /* 清 TX FIFO */
#define ESP32C3_UART_FCR_TRG1   (0u << 6)   /* RX 触发阈值 1 字节 */
#define ESP32C3_UART_LCR_WLEN8  (3u << 0)   /* 8 位字长 */
#define ESP32C3_UART_LCR_DLAB   (1u << 7)   /* 分频锁存访问 */
#define ESP32C3_UART_LSR_DR     (1u << 0)   /* 接收数据就绪 */
#define ESP32C3_UART_LSR_THRE   (1u << 5)   /* 发送保持空 */
#define ESP32C3_UART_LSR_TEMT   (1u << 6)   /* 发送器空 */

/* 波特率分频（16550：divisor = 时钟 / (16 * baud)；Renode 模型不校验，给 115200
 * 一个合法分频值即可。Phase-1 用 1.8432 MHz 内部时钟惯用值。 */
#define ESP32C3_UART_BAUD_DIV(baud) ((1843200u / 16u) / (uint32_t)(baud))

#endif /* JOC_DEVICE_ESP32C3_H */
