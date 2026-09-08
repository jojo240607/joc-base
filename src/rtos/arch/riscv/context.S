/*
 * RISC-V (RV32IMC) 上下文切换汇编（arch 层实现，ISA 相关）
 *
 *   _trap_handler      ：mtvec 单入口。RISC-V 异常/中断【不会】硬件自动压栈
 *                        （与 Cortex-M 不同），故入口处必须把全部整数寄存器
 *                        连同 mepc/mstatus 保存到当前栈上（128 B 帧），再按
 *                        mcause 分发：
 *                          - 机器定时器中断   : 重装 mtimecmp + 分发节拍
 *                          - 机器软件中断     : 调度切换（PendSV 对等物）
 *                          - 机器外部中断     : PLIC claim/complete + irq_dispatch
 *                          - M 模式 ecall     : 首次启动第一个任务
 *                          - U 模式 ecall     : 非特权任务系统调用门
 *                          - 其它异常         : 记录并停机（rtos_riscv_fault_handler）
 *                        最后统一恢复帧并 mret。
 *
 * 帧布局与 src/rtos/arch/riscv/riscv.h 的 RISCV_FRAME_* 常量严格一致：
 *   低地址 → 高地址：x1,x3..x31（30 字）, mepc(0x78), mstatus(0x7C)，共 0x80 B。
 *   t->sp 指向帧基址；恢复后 sp += 0x80 再 mret。
 *
 * 中断模型：入口不重开 MIE（单层中断，无嵌套），trap 期间 g_riscv_in_trap=1
 * 标记“处于中断上下文”（arch_in_isr 读取；用 RAM 全局而非 mscratch，因 U 模式
 * 任务读 mscratch 会触发 Illegal instruction）；退出前清零。mret 由硬件按帧内
 * mstatus 的 MPIE/MPP 恢复 MIE 与特权级。
 */
    .section .text

/* ---------------------------------------------------------------------------
 * _trap_handler —— 所有异常/中断的唯一入口（mtvec 直连）
 * ------------------------------------------------------------------------- */
    .global _trap_handler
    .type _trap_handler, %function
    .align 4
_trap_handler:
    addi  sp, sp, -0x80
    sw    x1,  0x00(sp)          /* ra  */
    sw    x3,  0x04(sp)          /* gp  */
    sw    x4,  0x08(sp)          /* tp  */
    sw    x5,  0x0C(sp)          /* t0  */
    sw    x6,  0x10(sp)          /* t1  */
    sw    x7,  0x14(sp)          /* t2  */
    sw    x8,  0x18(sp)          /* s0  */
    sw    x9,  0x1C(sp)          /* s1  */
    sw    x10, 0x20(sp)          /* a0  */
    sw    x11, 0x24(sp)          /* a1  */
    sw    x12, 0x28(sp)          /* a2  */
    sw    x13, 0x2C(sp)          /* a3  */
    sw    x14, 0x30(sp)          /* a4  */
    sw    x15, 0x34(sp)          /* a5  */
    sw    x16, 0x38(sp)          /* a6  */
    sw    x17, 0x3C(sp)          /* a7  */
    sw    x18, 0x40(sp)          /* s2  */
    sw    x19, 0x44(sp)          /* s3  */
    sw    x20, 0x48(sp)          /* s4  */
    sw    x21, 0x4C(sp)          /* s5  */
    sw    x22, 0x50(sp)          /* s6  */
    sw    x23, 0x54(sp)          /* s7  */
    sw    x24, 0x58(sp)          /* s8  */
    sw    x25, 0x5C(sp)          /* s9  */
    sw    x26, 0x60(sp)          /* s10 */
    sw    x27, 0x64(sp)          /* s11 */
    sw    x28, 0x68(sp)          /* t3  */
    sw    x29, 0x6C(sp)          /* t4  */
    sw    x30, 0x70(sp)          /* t5  */
    sw    x31, 0x74(sp)          /* t6  */
    csrr  t0, mepc
    sw    t0, 0x78(sp)
    csrr  t0, mstatus
    sw    t0, 0x7C(sp)

    /* 诊断：记录本次 trap 的 mcause/mepc 与帧基址到环形缓冲（RISC-V 调试）
     * t0/t1/t2 已入帧，此处可安全复用。每项 3 字：mcause, mepc, frame_sp。 */
    la    t0, g_riscv_trap_ring_cnt
    lw    t1, 0(t0)
    andi  t1, t1, 7
    slli  t1, t1, 4              /* 每项 3 字 = 0xC，对齐 4 用 0x10 */
    la    t2, g_riscv_trap_ring
    add   t2, t2, t1
    csrr  t1, mcause
    sw    t1, 0(t2)
    csrr  t1, mepc
    sw    t1, 4(t2)
    sw    sp, 8(t2)
    la    t0, g_riscv_trap_ring_cnt
    lw    t1, 0(t0)
    addi  t1, t1, 1
    sw    t1, 0(t0)

    li    t0, 1
    la    t1, g_riscv_in_trap
    sw    t0, 0(t1)              /* 中断上下文标记（arch_in_isr 读 g_riscv_in_trap） */

    csrr  t0, mcause
    li    t1, 0x80000007         /* 机器定时器中断 */
    beq   t0, t1, .Ltrap_timer
    li    t1, 0x80000003         /* 机器软件中断（调度切换） */
    beq   t0, t1, .Ltrap_switch
    li    t1, 0x8000000B         /* 机器外部中断（PLIC） */
    beq   t0, t1, .Ltrap_external
    li    t1, 11                 /* M 模式 ecall：首次启动 */
    beq   t0, t1, .Ltrap_ecall_m
    li    t1, 8                  /* U 模式 ecall：系统调用 */
    beq   t0, t1, .Ltrap_ecall_u
    j     .Ltrap_fault

/* ---- 节拍：重装 mtimecmp 后分发（与 RTOS tick / systick 驱动共享 id 7） ---- */
.Ltrap_timer:
    call  rtos_riscv_tick_trap
    j     .Ltrap_restore

/* ---- 软件中断：清 msip，做实际上下文切换（PendSV 对等物） ---- */
.Ltrap_switch:
    li    t0, 0x02000000         /* CLINT 基址：msip @ +0 */
    sw    zero, 0(t0)            /* 清软件中断挂起 */
    mv    a0, sp                 /* old_sp = 当前任务帧基址 */
    call  rtos_pendsv_switch     /* 返回新任务 sp；g_running = nxt */
    mv    sp, a0
    call  rtos_arch_apply_task_priv   /* 按新任务 priv 写帧内 mstatus.MPP */
    j     .Ltrap_restore

/* ---- 外部中断：PLIC claim → 框架分发 → complete ---- */
.Ltrap_external:
    call  IRQ_CommonHandler
    j     .Ltrap_restore

/* ---- M 模式 ecall：仅用于首次启动第一个任务 ---- */
.Ltrap_ecall_m:
    la    t0, g_rtos_psp_ready
    lw    t0, 0(t0)
    bnez  t0, .Ltrap_fault        /* 首切之后出现 M 模式 ecall：视为故障 */
    la    t0, g_running
    lw    t0, 0(t0)
    lw    sp, 0(t0)               /* sp = 首任务初始帧基址（task.c 构造） */
    la    t0, g_rtos_psp_ready
    li    t1, 1
    sw    t1, 0(t0)               /* 解锁 rtos_schedule_request 首切前封锁 */
    call  rtos_arch_apply_task_priv
    j     .Ltrap_restore

/* ---- U 模式 ecall：系统调用门（非特权任务经 rtos_syscall 触发） ----
 * 调用号/参数在帧内 a0..a3（偏移 0x20/0x24/0x28/0x2C）。rtos_svc_dispatch 按
 * “frame[0..3] = nr,arg0,arg1,arg2” 的 ARM 约定工作，故在此先拷出 4 字
 * 参数帧，分发后把返回值写回真实帧的 a0 槽（随 mret 带回任务）。
 * 返回前必须把帧内 mepc += 4：ecall 的 mepc 指向 ecall 指令自身，不跳过
 * 则 mret 重新执行 ecall 死循环（见下方 .Ltrap_ecall_u 末尾）。 */
.Ltrap_ecall_u:
    addi  sp, sp, -16
    lw    t0, 0x30(sp)            /* 真实帧 a0 = 调用号 (0x20 + 0x10) */
    sw    t0, 0(sp)
    lw    t0, 0x34(sp)            /* a1 */
    sw    t0, 4(sp)
    lw    t0, 0x38(sp)            /* a2 */
    sw    t0, 8(sp)
    lw    t0, 0x3C(sp)            /* a3 */
    sw    t0, 12(sp)
    mv    a0, sp
    lw    a1, 0(sp)
    call  rtos_svc_dispatch_entry
    lw    t0, 0(sp)               /* 返回值 */
    addi  sp, sp, 16
    sw    t0, 0x20(sp)            /* 写回真实帧 a0 */
    /* U 模式 ecall 的 mepc 指向 ecall 指令自身（RISC-V 不自动跳过），
     * 必须 +4 使其回到 ecall 后的 `ret`，否则 mret 会重新执行 ecall
     * 造成死循环（RTOSUSR 首个 SVC 即卡死，主任务超时全 FAIL）。 */
    lw    t0, 0x78(sp)
    addi  t0, t0, 4
    sw    t0, 0x78(sp)
    j     .Ltrap_restore

/* ---- 其它异常：交给 C 侧故障处理（记录后停机，永不返回） ---- */
.Ltrap_fault:
    csrr  a0, mcause
    csrr  a1, mepc
    csrr  a2, mtval
    call  rtos_riscv_fault_handler
    j     .Ltrap_restore          /* 不可达（fault handler 自旋） */

/* ---- 统一恢复：mepc/mstatus 先写（借 t0，随后 GPR 块会恢复 t0 本身），
 *      再恢复全部整数寄存器，sp 加回帧长后 mret ---- */
.Ltrap_restore:
    la    t0, g_riscv_in_trap
    sw    zero, 0(t0)            /* 清中断上下文标记 */
    lw    t0, 0x78(sp)
    csrw  mepc, t0
    lw    t0, 0x7C(sp)
    csrw  mstatus, t0
    lw    x1,  0x00(sp)
    lw    x3,  0x04(sp)
    lw    x4,  0x08(sp)
    lw    x5,  0x0C(sp)
    lw    x6,  0x10(sp)
    lw    x7,  0x14(sp)
    lw    x8,  0x18(sp)
    lw    x9,  0x1C(sp)
    lw    x10, 0x20(sp)
    lw    x11, 0x24(sp)
    lw    x12, 0x28(sp)
    lw    x13, 0x2C(sp)
    lw    x14, 0x30(sp)
    lw    x15, 0x34(sp)
    lw    x16, 0x38(sp)
    lw    x17, 0x3C(sp)
    lw    x18, 0x40(sp)
    lw    x19, 0x44(sp)
    lw    x20, 0x48(sp)
    lw    x21, 0x4C(sp)
    lw    x22, 0x50(sp)
    lw    x23, 0x54(sp)
    lw    x24, 0x58(sp)
    lw    x25, 0x5C(sp)
    lw    x26, 0x60(sp)
    lw    x27, 0x64(sp)
    lw    x28, 0x68(sp)
    lw    x29, 0x6C(sp)
    lw    x30, 0x70(sp)
    lw    x31, 0x74(sp)
    addi  sp, sp, 0x80
    mret

    .size _trap_handler, . - _trap_handler
