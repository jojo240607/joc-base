#ifndef JOC_RTOS_ARCH_RISCV_H
#define JOC_RTOS_ARCH_RISCV_H

/* ===========================================================================
 * RISC-V (RV32IMC) 移植旋钮头（arch 层内部使用，不暴露给可移植核心）
 *
 * 目标：ESP32-C3（RV32IMC，无 A/F 扩展）在 Renode 上的 Phase-1 仿真。
 *   - mtvec 单入口 _trap_handler（Direct 模式），mcause 分发；
 *   - CLINT（Renode CoreLevelInterruptor @ 0x02000000）提供 msip/mtimecmp/mtime；
 *   - PLIC（Renode PlatformLevelInterruptController @ 0x0C000000）分发外设中断；
 *   - 无原子指令（RV32I）：lock.h / atomic.h 用 MIE 临界区近似（单核安全）。
 *
 * 帧布局（32 字 = 128 B，context.S 保存 / task.c 初始帧 / port.c 统一使用）：
 *   word 0..29 : x1(x3..x31) 全部整数寄存器（x0 恒 0 不存）
 *   word 30    : mepc
 *   word 31    : mstatus
 *   t->sp 恒指向帧基址（最低地址）；恢复时 sp = 帧基址 + 128 后 mret。
 * ========================================================================= */

#include <stdint.h>

/* ---- 机器模式 CSR 编号（便于 C 侧内联汇编使用） ---- */
#define RISCV_CSR_MSTATUS   0x300u
#define RISCV_CSR_MISA      0x301u
#define RISCV_CSR_MIE       0x304u
#define RISCV_CSR_MTVEC     0x305u
#define RISCV_CSR_MSCRATCH  0x340u
#define RISCV_CSR_MEPC      0x341u
#define RISCV_CSR_MCAUSE    0x342u
#define RISCV_CSR_MTVAL     0x343u
#define RISCV_CSR_MIP       0x344u
#define RISCV_CSR_MCYCLE    0xB00u

/* ---- mstatus 位 ---- */
#define RISCV_MSTATUS_MIE      (1u << 3)     /* 全局中断使能（PRIMASK 对等物） */
#define RISCV_MSTATUS_MPIE     (1u << 7)     /* 上次中断前的 MIE（mret 恢复用） */
#define RISCV_MSTATUS_MPP_SHIFT 11u
#define RISCV_MSTATUS_MPP_MASK (3u << 11)    /* 上次特权级：0=U, 3=M */
#define RISCV_MSTATUS_MPP_U    (0u << 11)
#define RISCV_MSTATUS_MPP_M    (3u << 11)

/* ---- mie / mip 中断位 ---- */
#define RISCV_IRQ_SOFT  3u                   /* 机器软件中断（调度请求） */
#define RISCV_IRQ_TIMER 7u                   /* 机器定时器中断（节拍） */
#define RISCV_IRQ_EXT   11u                  /* 机器外部中断（PLIC） */
#define RISCV_MIE_ALL   ((1u << RISCV_IRQ_SOFT) | (1u << RISCV_IRQ_TIMER) | \
                         (1u << RISCV_IRQ_EXT))

/* ---- mcause ---- */
#define RISCV_MCAUSE_INT      (1u << 31)     /* 1 = 中断, 0 = 异常 */
#define RISCV_MCAUSE_CODE     (0x7FFFFFFFu)
#define RISCV_MCAUSE_ECALL_U  8u             /* U 模式环境调用（系统调用门） */
#define RISCV_MCAUSE_ECALL_M  11u            /* M 模式环境调用（首次启动） */

/* ---- 上下文帧布局（context.S 与 port.c/task.c 必须一致） ---- */
#define RISCV_FRAME_RA        0x00u   /* x1  */
#define RISCV_FRAME_GP        0x04u   /* x3  */
#define RISCV_FRAME_TP        0x08u   /* x4  */
#define RISCV_FRAME_T0        0x0Cu   /* x5  */
#define RISCV_FRAME_T1        0x10u   /* x6  */
#define RISCV_FRAME_T2        0x14u   /* x7  */
#define RISCV_FRAME_S0        0x18u   /* x8  */
#define RISCV_FRAME_S1        0x1Cu   /* x9  */
#define RISCV_FRAME_A0        0x20u   /* x10 */
#define RISCV_FRAME_A1        0x24u   /* x11 */
#define RISCV_FRAME_A2        0x28u   /* x12 */
#define RISCV_FRAME_A3        0x2Cu   /* x13 */
#define RISCV_FRAME_A4        0x30u   /* x14 */
#define RISCV_FRAME_A5        0x34u   /* x15 */
#define RISCV_FRAME_A6        0x38u   /* x16 */
#define RISCV_FRAME_A7        0x3Cu   /* x17 */
#define RISCV_FRAME_S2        0x40u   /* x18 */
#define RISCV_FRAME_S3        0x44u   /* x19 */
#define RISCV_FRAME_S4        0x48u   /* x20 */
#define RISCV_FRAME_S5        0x4Cu   /* x21 */
#define RISCV_FRAME_S6        0x50u   /* x22 */
#define RISCV_FRAME_S7        0x54u   /* x23 */
#define RISCV_FRAME_S8        0x58u   /* x24 */
#define RISCV_FRAME_S9        0x5Cu   /* x25 */
#define RISCV_FRAME_S10       0x60u   /* x26 */
#define RISCV_FRAME_S11       0x64u   /* x27 */
#define RISCV_FRAME_T3        0x68u   /* x28 */
#define RISCV_FRAME_T4        0x6Cu   /* x29 */
#define RISCV_FRAME_T5        0x70u   /* x30 */
#define RISCV_FRAME_T6        0x74u   /* x31 */
#define RISCV_FRAME_MEPC      0x78u   /* word 30 */
#define RISCV_FRAME_MSTATUS   0x7Cu   /* word 31 */
#define RISCV_FRAME_SIZE      0x80u   /* 32 字 */

/* 初始帧 mstatus：MPIE=1（首次 mret 后 MIE=1）、MPP=M（特权任务），
 * MIE=0（mret 忽略 MIE，只看 MPIE）。非特权任务由 apply_task_priv 改写 MPP。 */
#define RISCV_FRAME_MSTATUS_INIT (RISCV_MSTATUS_MPIE | RISCV_MSTATUS_MPP_M)

/* ---- CLINT（Renode RiscV32 机器定时器/软件中断控制器） ---- */
#define RISCV_CLINT_BASE     0x02000000u
#define RISCV_CLINT_MSIP     0x02000000u   /* 写 1 触发机器软件中断 */
#define RISCV_CLINT_MTIMECMP 0x02004000u   /* 64 位比较值（低字 +4 高字） */
#define RISCV_CLINT_MTIME    0x0200BFF8u   /* 64 位实时计数器 */

/* ---- PLIC（Renode 平台级中断控制器，1 上下文） ---- */
#define RISCV_PLIC_BASE      0x0C000000u
#define RISCV_PLIC_PRIORITY  0x0C000000u   /* PRIORITY[src] = base + 4*src */
#define RISCV_PLIC_ENABLE    0x0C002000u   /* 位 (src-1) 置 1 使能源 src */
#define RISCV_PLIC_THRESHOLD 0x0C200000u   /* 全局阈值（0 = 全放行） */
#define RISCV_PLIC_CLAIM     0x0C200004u   /* 读 = 取最高优先挂起源（清 pending） */
#define RISCV_PLIC_COMPLETE  0x0C200004u   /* 写回源号 = 完成（可再次响应） */

/* 外设中断源 → irq id 映射：PLIC 源 s (1..31) ↔ irq id (16 + s)。
 * id 0..15 保留给核心异常（3=软件调度, 7=机器定时器节拍）。 */
#define RISCV_IRQ_EXTERNAL_BASE 16u

/* ---- CSR 内联辅助（M 模式；Phase-1 任务均为特权态，可安全执行） ---- */
static inline uint32_t riscv_csr_read(uint32_t csr)
{
    uint32_t v;
    __asm__ volatile("csrr %0, %1" : "=r"(v) : "i"(csr));
    return v;
}

static inline void riscv_csr_write(uint32_t csr, uint32_t v)
{
    __asm__ volatile("csrw %0, %1" :: "i"(csr), "r"(v));
}

static inline void riscv_csr_set(uint32_t csr, uint32_t bits)
{
    __asm__ volatile("csrs %0, %1" :: "i"(csr), "r"(bits) : "memory");
}

static inline void riscv_csr_clear(uint32_t csr, uint32_t bits)
{
    __asm__ volatile("csrc %0, %1" :: "i"(csr), "r"(bits) : "memory");
}

/* trap 上下文标记（port.c 定义，context.S 入口置 1 / 恢复前清零）。
 * 供 lock.h 的 arch_in_isr() 使用：U 模式任务可安全读普通 RAM 全局，
 * 而读 M 模式 CSR（mscratch）在 U 模式会触发 Illegal instruction。 */
extern volatile uint32_t g_riscv_in_trap;

/* 当前是否特权（M 模式）上下文：trap 处理器运行于 M 模式恒特权；
 * 任务上下文按 g_running->priv 判定（见 port.c 实现说明）。
 * 不能用 mstatus.MPP：mret 会把 MPP 清为 U，任务执行期间读 MPP 恒为 0。 */
int rtos_arch_in_priv(void);

#endif /* JOC_RTOS_ARCH_RISCV_H */
