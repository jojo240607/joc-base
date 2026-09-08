#include "rtos.h"
#include "rtos_internal.h"      /* g_running / g_rtos_started / g_rtos_psp_ready */
#include "riscv.h"
#include "irq.h"                /* irq_dispatch(irq_id_t) */
#include "rtos_config.h"        /* RTOS_TICK_HZ */
#include <stdint.h>

/* ===========================================================================
 * RISC-V (RV32IMC) arch 移植层 C 侧（对 rtos_arch.h 的 8 函数契约实现）
 *
 * 目标：ESP32-C3 在 Renode 上的 Phase-1 仿真。
 *   - mtvec 直连 _trap_handler（context.S），所有异常/中断单入口；
 *   - CLINT（Renode CoreLevelInterruptor @ 0x02000000，10 MHz）提供
 *     msip（软件中断 = PendSV 对等物）、mtimecmp/mtime（机器定时器 = 节拍）；
 *   - PLIC（Renode PlatformLevelInterruptController @ 0x0C000000）外设中断
 *     由 irq_hal.c 的 IRQ_CommonHandler 接管（PLIC claim/complete）；
 *   - mcycle CSR 恒计数（无需使能），作为周期计数器（DWT CYCCNT 对等物）。
 *
 * 帧布局与 riscv.h / context.S / task.c 严格一致：t->sp 指向 32 字
 * （0x80 B）帧基址，帧内 mstatus@0x7C / mepc@0x78 / a0@0x20 / ra@0x00。
 * ========================================================================= */

/* CLINT 输入时钟频率（Renode CoreLevelInterruptor frequency: 10000000）。
 * 节拍周期 = RISCV_CLINT_HZ / RTOS_TICK_HZ = 10000。 */
#define RISCV_CLINT_HZ 10000000u

extern void _trap_handler(void);    /* context.S 的 mtvec 单入口 */

/* RISC-V 调试：最近 8 次 trap 的 (mcause, mepc, 帧基址 sp)。context.S 入口处
 * 按 0x10 步长写 3 字/项（见 context.S 环形缓冲段），数组必须给足 3 列，否则
 * 第 7 项的 `sw sp, 8(t2)` 会越界写坏相邻 .bss 符号（此前 8x2 布局曾破坏
 * g_riscv_trap_ring_cnt 及 g_running 等，导致调度器状态被踩）。 */
volatile uint32_t g_riscv_trap_ring[8][3];
volatile uint32_t g_riscv_trap_ring_cnt;

/* trap 上下文标记（context.S 入口置 1、恢复前清零）：替代 mscratch 供
 * arch_in_isr() 探测。必须在 RAM 中而非 M 模式 CSR——U 模式任务读 mscratch
 * 会触发 Illegal instruction（mcause=2），而读普通全局变量天然安全。 */
volatile uint32_t g_riscv_in_trap;

/* ---------------------------------------------------------------------------
 * 调度请求（PendSV 对等物）
 * ------------------------------------------------------------------------- */
void rtos_schedule_request(void) {
    /* 与 Cortex-M 相同的双闸门：调度器未启动 / 首任务尚未切换（g_rtos_psp_ready
     * 未置位）时写 msip 会让软件中断在“首任务 mret 之后”立刻触发，而那时
     * _trap_handler 会按“正常切换”路径保存当前任务帧——但首切前 g_running 的
     * sp 还是初始帧（未运行过），会破坏首任务启动。故首切前一律退回。 */
    if (!g_rtos_started)  return;
    if (!g_rtos_psp_ready) return;
    *(volatile uint32_t *)RISCV_CLINT_MSIP = 1u;   /* 置位机器软件中断挂起 */
}

/* ---------------------------------------------------------------------------
 * 启动调度：装 mtvec、开 mie、ecall 触发首任务切换
 * ------------------------------------------------------------------------- */
void rtos_arch_start(void) {
    riscv_csr_write(RISCV_CSR_MTVEC, (uint32_t)&_trap_handler);
    /* 使能全部机器中断（软件/定时器/外部）。ecall 前保持 MIE=1：
     * ecall 异常会把“进入前 MIE=1”保存为 MPIE=1，但首任务 mret 用的是
     * task.c 构造的初始帧（RISCV_FRAME_MSTATUS_INIT：MPIE=1, MPP=M），
     * 与当前 mstatus 无关——这里置 MIE 仅为保持状态一致。 */
    riscv_csr_set(RISCV_CSR_MIE, RISCV_MIE_ALL);
    riscv_csr_set(RISCV_CSR_MSTATUS, RISCV_MSTATUS_MIE);
    __asm__ volatile("ecall" ::: "memory");   /* M 模式 ecall -> .Ltrap_ecall_m */
    for (;;) { }   /* 不会返回 */
}

/* ---------------------------------------------------------------------------
 * 节拍：机器定时器中断 id（RTOS tick 经 irq 框架注册到该 id）
 * ------------------------------------------------------------------------- */
irq_id_t rtos_arch_tick_id(void) {
    return (irq_id_t)RISCV_IRQ_TIMER;   /* 7 */
}

/* 重装 mtimecmp = mtime + 节拍周期（64 位 CLINT 访问：先写低字再写高字）。
 * 从 mtime 读当前值而非在旧 cmp 上加周期：补偿中断响应期间流逝的时间，
 * 避免长时间关中断后 mtimecmp 已过期导致中断风暴（与 OpenSBI 做法一致）。 */
void rtos_arch_tick_start(void) {
    uint32_t lo = *(volatile uint32_t *)RISCV_CLINT_MTIME;
    uint32_t hi = *(volatile uint32_t *)(RISCV_CLINT_MTIME + 4u);
    uint64_t now = ((uint64_t)hi << 32) | lo;
    uint64_t next = now + (uint64_t)(RISCV_CLINT_HZ / (uint32_t)RTOS_TICK_HZ);
    *(volatile uint32_t *)RISCV_CLINT_MTIMECMP      = (uint32_t)(next & 0xFFFFFFFFu);
    *(volatile uint32_t *)(RISCV_CLINT_MTIMECMP + 4u) = (uint32_t)(next >> 32);
}

/* 节拍 trap 处理（context.S .Ltrap_timer 调）：重装 mtimecmp 后经 irq 框架
 * 分发 id=7，命中 rtos_init 注册的 rtos_tick_isr（与 Cortex-M SysTick 同构）。 */
void rtos_riscv_tick_trap(void) {
    rtos_arch_tick_start();
    irq_dispatch((irq_id_t)RISCV_IRQ_TIMER);
}

/* ---------------------------------------------------------------------------
 * 周期计数器：mcycle CSR 恒运行（RV32I 无 DWT 需要使能），读即可
 * ------------------------------------------------------------------------- */
void rtos_cycle_init(void) {
    /* mcycle 自复位即计数，无使能寄存器；幂等 NOP。 */
}
uint32_t rtos_cycle_now(void) {
    return riscv_csr_read(RISCV_CSR_MCYCLE);
}

/* ---------------------------------------------------------------------------
 * 按新任务的 priv 标志改写其帧内 mstatus.MPP（context.S 在切换点调用）。
 * RISC-V 没有 CONTROL.nPRIV 寄存器：任务特权级由 mret 时的 mstatus.MPP
 * 决定（0=U, 3=M）。利用契约保证——rtos_pendsv_switch 返回时
 * g_running->sp == 新任务帧基址（旧任务帧已由 asm 保存、新任务帧已载入 sp），
 * 因此无需任何参数即可定位新任务帧；非特权任务改写为 U，特权任务为 M。
 * 注意：只能在 trap 上下文（PendSV/SVC 对等物）调用——帧基址仅在此刻有效。 */
extern task_t *g_running;
void rtos_arch_apply_task_priv(void) {
    if (!g_running) return;
    uint32_t *frame = (uint32_t *)g_running->sp;
    uint32_t ms = frame[RISCV_FRAME_MSTATUS / 4u];
    ms &= ~RISCV_MSTATUS_MPP_MASK;
    ms |= g_running->priv ? RISCV_MSTATUS_MPP_M : RISCV_MSTATUS_MPP_U;
    frame[RISCV_FRAME_MSTATUS / 4u] = ms;
}

/* 当前是否运行在非特权态（自测断言用）。
 * 不能读 mstatus 判断：U 模式读 M 模式 CSR 触发 Illegal instruction（mcause=2）。
 * 任务特权级由 rtos_arch_apply_task_priv 依据 g_running->priv 写入帧内 MPP，
 * 故 g_running->priv 即为当前运行任务的权威特权来源，且为普通 RAM 读、U/M 两态安全。 */
int rtos_arch_in_unpriv(void) {
    return (g_running && !g_running->priv) ? 1 : 0;
}

/* 当前是否特权（M 模式）上下文。RISC-V 无“当前模式”可读 CSR，且 mret 会把
 * mstatus.MPP 清为 U，任务执行期间读 MPP 恒得 0——不能据此判特权（此前实现
 * 使临界区审计在任务上下文恒被跳过）。改用软件判定：
 *   - trap 处理器（中断/SVC 上下文）运行于 M 模式，恒特权（读 mcycle 安全）；
 *   - 任务上下文按 g_running->priv（rtos_arch_apply_task_priv 依据它写帧内
 *     MPP，是任务特权的权威来源，且为普通 RAM 读、U/M 两态安全）。
 * 返回 0 时调用方（rtos_crit_enter_mark/exit_audit）跳过 mcycle 读，规避
 * U 模式读 M 模式 CSR 的 Illegal instruction。 */
int rtos_arch_in_priv(void) {
    if (g_riscv_in_trap) return 1;
    return (g_running && g_running->priv) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * 系统调用门（U 模式 ecall；SVC 对等物）
 * ------------------------------------------------------------------------- */
/* 非特权任务经此切到 M 模式执行内核 API（context.S .Ltrap_ecall_u 分发）。
 * 参数 a0..a3 已按 RISC-V ABI 就位；trap 处理把返回值写回帧内 a0 槽，
 * mret 后 a0 即返回值，`ret` 用不变的 ra 返回调用方。
 * RTOSUSR 自测会创建非特权任务（priv=0），rtos_need_svc() 在该上下文为真，
 * 经 ecall 走 SVC 门；特权任务恒直行，不触发此路径。 */
__attribute__((naked))
uint32_t rtos_syscall(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2) {
    __asm__ volatile("ecall\n ret" ::: "memory");
}

/* ---------------------------------------------------------------------------
 * 机器异常故障处理（context.S .Ltrap_fault 调；memfault 对等物）
 * ------------------------------------------------------------------------- */
volatile uint32_t g_riscv_fault_count;
volatile uint32_t g_riscv_fault_mcause;
volatile uint32_t g_riscv_fault_mepc;
volatile uint32_t g_riscv_fault_mtval;

void rtos_riscv_fault_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval) {
    g_riscv_fault_mcause = mcause;
    g_riscv_fault_mepc   = mepc;
    g_riscv_fault_mtval  = mtval;
    g_riscv_fault_count++;
    /* 记录后停机自旋：诊断信息经调试器 / Renode 读 g_riscv_fault_* 定位。
     * 不返回（context.S 的后续 restore 不可达）。 */
    for (;;) { }
}
