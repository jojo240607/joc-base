/*
 * Cortex-M4 上下文切换汇编（arch 层实现，ISA 相关）
 *  - PendSV_Handler：保存/恢复 r4-r11、EXC_RETURN，以及 FPU 的 S16-S31（若任务用 FPU），
 *                    调用 C 侧选下一任务。
 *  - SVC_Handler   ：首次切换到第一个任务（从线程模式 MSP 切到 PSP）。
 *  - MemManage / HardFault_Handler：把异常帧交给 C 侧 rtos_fault_handler。
 *
 * 这些符号覆盖 startup 向量表里的弱符号 PendSV_Handler / SVC_Handler /
 * MemManage_Handler / HardFault_Handler。
 *
 * 本文件是“可移植内核核心”与底层之间的隔离层之一：它只依赖 ARMv7-M 指令集
 * 与 Cortex-M 异常模型，不碰任何芯片外设。
 *
 * FPU 上下文：带 FPU 的 Cortex-M4 中，异常入口硬件会对 S0-S15 + FPSCR 做“懒栈”
 * （EXC_RETURN[4]=0 表示扩展帧，FPU 处于活动态）。这部分由硬件自动压/取，无需手动
 * 处理。但 S16-S31 是 callee-saved 且跨上下文共享，必须像 r4-r11 一样手动保存/恢复。
 * 是否保存由 EXC_RETURN 的 bit4 决定：bit4=0（扩展帧，任务用过 FPU）才存 S16-S31，
 * 这样不用的任务零开销。无 FPU 的型号（如 M0/M0+）请新建独立 arch 并去掉 FPU 段。
 */
    .syntax unified
    .thumb

/* 移植旋钮（与 cortex_m.h 的 __FPU_PRESENT 对齐；无 FPU 的 arch 变体应 -D__FPU_PRESENT=0） */
#ifndef __FPU_PRESENT
  #define __FPU_PRESENT 1
#endif

    .section .text

    .global PendSV_Handler
    .global SVC_Handler
    /* 标记为 Thumb 函数，使链接器在向量表里写入地址时置 bit0=1，
     * 否则 CPU 取到偶数地址会触发 INVSTATE HardFault。 */
    .type PendSV_Handler, %function
    .thumb_func
    .type SVC_Handler, %function
    .thumb_func

/* ---------------------------------------------------------------------------
 * PendSV：最低优先级异常，做实际切换。
 *   进入时 lr = EXC_RETURN（由上次异常返回压入的约定值）。
 *   硬件已在异常入口把 r0-r3,r12,lr,pc,xPSR 压到当前栈（PSP）；若任务用过 FPU，
 *   还会懒栈保留 S0-S15 + FPSCR 的 0x48 空间（EXC_RETURN[4]=0）。
 *   这里手动保存/恢复 r4-r11、EXC_RETURN，以及（用过 FPU 时）S16-S31。
 * ------------------------------------------------------------------------- */
PendSV_Handler:
    mrs    r0, PSP

    /* 调试录制：记录进入时的 EXC_RETURN / CONTROL / ICSR，用于定位 boot HardFault
     * （判定是否从 Handler 模式进入、被切出任务 FPU 帧类型）。r1/r2 非被保存寄存器，
     * 后续 SAVE 只动 r0/r3/r4-r11，故此处破坏 r1/r2 安全。 */
    ldr    r2, =g_pendsv_enter_lr
    str    lr, [r2]
    ldr    r2, =g_pendsv_enter_ctrl
    mrs    r1, CONTROL
    str    r1, [r2]
    ldr    r2, =g_pendsv_enter_icsr
    ldr    r1, =0xE000ED04
    ldr    r1, [r1]
    str    r1, [r2]

    /* 判定【被切出任务】是否用过 FPU：用 PendSV 的 lr(EXC_RETURN) 的 bit4，与下方
     * RESTORE 路径严格对称（RESTORE 同样用 tst r14,#0x10）。这是 Cortex-M 异常帧的
     * 标准判定法：PendSV 进入时 r14 即“被切出任务被中断时的 EXC_RETURN”，无论 PendSV
     * 是从线程模式还是从某 ISR 链条进入，该值都反映被切出任务的帧类型（bit4=0=扩展帧/
     * 用过 FPU，bit4=1=基本帧/没用过 FPU）。SAVE 与 RESTORE 必须用同一判据，否则帧大小
     * 错位(差 0x40) -> 恢复出垃圾 PC -> IBUSERR/HardFault。
     * 注意：不能用 FPCCR.LSPACT 判定——它反映的是“懒栈是否仍挂起”，在 PendSV 真正
     * 执行时懒栈可能已被冲刷，与被切出任务真实帧类型不一致。 */
#if __FPU_PRESENT
    tst    r14, #0x10         /* bit4? 0=扩展帧(用过 FPU) 1=基本帧(没用过) */
    beq    .Lpendsv_fpu_save  /* bit4==0 -> 保存 S16-S31 */
    /* 基本帧：没用过 FPU。布局低→高 = [EXC_RETURN=0xFFFFFFFD][r4-r11][hw帧] */
    stmdb  r0!, {r4-r11}
    mov    r3, #0xFFFFFFFD
    str    r3, [r0, #-4]!    /* EXC_RETURN(word0) 在最低地址 */
    b      .Lpendsv_saved
.Lpendsv_fpu_save:
    /* 扩展帧：用过 FPU。先 vstmdb 把 s16-s31 压到栈顶（紧邻硬件异常帧），
     * 这同时把硬件懒栈的 S0-S15+FPSCR flush 到本任务栈，使整个 FPU 上下文完整。
     * 布局低→高 = [EXC_RETURN=0xFFFFFFED][r4-r11][s16-s31][hw帧] */
    vstmdb r0!, {s16-s31}
    stmdb  r0!, {r4-r11}
    mov    r3, #0xFFFFFFED
    str    r3, [r0, #-4]!    /* EXC_RETURN(word0) 在最低地址 */
.Lpendsv_saved:
#else
    /* 无 FPU：布局低→高 = [EXC_RETURN][r4-r11][hw帧] */
    stmdb  r0!, {r4-r11}
    stmdb  r0!, {r14}        /* EXC_RETURN 在最底部（最低地址） */
#endif

    /* 临界区已收口到 rtos_pendsv_switch 内部（rtos_crit_enter/exit：
     * ZERO_LATENCY>0 时 BASEPRI 选择性屏蔽，否则 PRIMASK），故此处不再粗暴关全中断。
     * 寄存器保存/恢复与 FPU 懒栈本来就运行在中断开启状态，只有“选下一任务”这一小段被保护。 */
    bl     rtos_pendsv_switch /* r0 = 新任务的 sp（PSP）；g_running = nxt */

    /* 保存 PSP 到 r4（callee-saved，C 函数 rtos_arch_apply_task_priv 会保留它）：
     * 该 C 函数可自由使用 r0-r3，会把 r0(此处即 PSP 栈帧指针) 改掉，故必须先存起来，
     * 否则下方 ldmia r0! 会从垃圾地址取 EXC_RETURN -> BusFault。 */
    mov    r4, r0
    bl     rtos_arch_apply_task_priv   /* 按新任务 priv 设 nPRIV + 重编程 R4 栈 region */
    mov    r0, r4            /* 恢复 PSP 栈帧指针 */
    ldmia  r0!, {r14}        /* 先恢复 EXC_RETURN（最低地址） */
    /* 恢复顺序必须与 SAVE 严格对称（SAVE 低→高 = EXC_RETURN, r4-r11, s16-s31）。
     * 注意：用过 FPU 的任务其帧中 r4-r11 位于 s16-s31 之下，故先恢复 r4-r11 再
     * 恢复 s16-s31；基本帧（bit4=1，没用过 FPU）则跳过 s16-s31。 */
#if __FPU_PRESENT
    tst    r14, #0x10
    bne    .Lno_fpu_restore  /* bit4=1：基本帧，任务没用过 FPU，仅恢复 r4-r11 */
    ldmia  r0!, {r4-r11}     /* FPU 任务：先恢复 r4-r11（在 s16-s31 之下） */
    vldmia r0!, {s16-s31}    /* 再恢复 s16-s31（帧顶部，紧邻硬件异常帧） */
    b      .Lrestore_done
.Lno_fpu_restore:
    ldmia  r0!, {r4-r11}     /* 无 FPU：仅恢复 r4-r11 */
.Lrestore_done:
#else
    ldmia  r0!, {r4-r11}     /* 恢复 r4-r11（在 EXC_RETURN 之上） */
#endif

    msr    PSP, r0
    bx     r14                /* 以 EXC_RETURN 返回到新任务（硬件据 bit4 决定是否取回 S0-S15/FPSCR） */

/* ---------------------------------------------------------------------------
 * SVC：仅用于“启动第一个任务”这一次。
 *   g_running 已由 rtos_start() 置为最高优先级就绪任务；此处把它装到 PSP，
 *   切到 PSP（线程模式），恢复 r4-r11，并以 EXC_RETURN=0xFFFFFFFD 返回。
 * ------------------------------------------------------------------------- */
SVC_Handler:
    /* 调试录制：无论走哪条分支，只要 SVC_Handler 被调用就记录入口 EXC_RETURN */
    ldr    r2, =g_svc_enter_lr
    str    lr, [r2]
    /* 取触发 SVC 的任务栈帧（线程模式用 PSP，主线程用 MSP），读取 SVC 指令的 imm8。
     * imm==0：首次启动首个任务（历史路径）；imm!=0：非特权任务的系统调用。 */
    tst    lr, #4
    ite    eq
    mrseq  r2, MSP
    mrsne  r2, PSP            /* r2 = 触发 SVC 的栈帧 */
    ldr    r3, [r2, #24]      /* 栈帧中保存的 PC（r0..r3,r12,lr,pc = 偏移 24） */
    ldrh   r3, [r3, #-2]      /* SVC 指令半字（PC 已指向下一条，故 -2） */
    and    r3, r3, #0xFF      /* imm8 */
    cbz    r3, .Lsvc_start    /* imm==0 -> 首次启动 */
    /* 系统调用路径：frame=r2, 调用号=frame[0]（rtos_syscall 把调用号放在 r0，
     * 而 SVC 指令固定为 svc 0x80，imm8 只是“这是系统调用”的标记，不是调用号；
     * 若误用 imm8 作调用号，switch 永远落 default -> 所有系统调用返回 -1，非特权
     * 任务的内核对象操作全部静默失效）。尾调用 C 分发器（lr 仍 = EXC_RETURN）。 */
    ldr    r1, =g_svc_syscall_path
    mov    r0, #0xAAAAu
    str    r0, [r1]
    mov    r0, r2
    ldr    r1, [r2, #0]       /* nr = frame[0] = 调用号 */
    b      rtos_svc_dispatch_entry
    ldr    r1, [r2, #0]       /* nr = frame[0] = 调用号 */
    b      rtos_svc_dispatch_entry

.Lsvc_start:
    /* 调试录制：首次启动路径进入时的 EXC_RETURN / CONTROL */
    ldr    r2, =g_svc_enter_lr
    str    lr, [r2]
    ldr    r2, =g_svc_enter_ctrl
    mrs    r1, CONTROL
    str    r1, [r2]
    ldr    r0, =g_running
    ldr    r0, [r0]
    ldr    r1, [r0]           /* first->sp（指向 EXC_RETURN 槽） */
    add    r1, r1, #4         /* 跳过 EXC_RETURN 槽（首次启动不用，PendSV 才用） */
    mov    r2, #2             /* CONTROL: SPSEL=1(用 PSP), nPRIV=0(暂定特权) */
    msr    CONTROL, r2
    isb
    ldmia  r1!, {r4-r11}      /* 弹出 r4-r11 */
    msr    PSP, r1            /* PSP 现在指向异常帧 */
    bl     rtos_arch_apply_task_priv   /* 按 first->priv 设 nPRIV（特权任务保持 0） */
    /* 首切完成：PSP 现在指向有效任务栈，后续任何 rtos_schedule_request（含 ISR 内）
     * 都可安全置 PENDSVSET。置位标志，解除 port.c 对首切之前的安全封锁。 */
    ldr    r2, =g_rtos_psp_ready
    mov    r1, #1
    str    r1, [r2]
    ldr    lr, =0xFFFFFFFD    /* 返回线程模式、用 PSP、无 FPU 帧 */
    bx     lr

/* ---------------------------------------------------------------------------
 * MemManage / HardFault：把异常帧交给 C 侧 rtos_fault_handler。
 *   根据 EXC_RETURN(lr) 的 bit2 选择栈帧所在（PSP=线程 / MSP=处理器）。
 * ------------------------------------------------------------------------- */
    .global MemManage_Handler
    .type MemManage_Handler, %function
    .thumb_func
MemManage_Handler:
    tst    lr, #4
    ite    eq
    mrseq  r0, MSP
    mrsne  r0, PSP
    mov    r1, lr           /* arg2 = EXC_RETURN */
    b      rtos_fault_handler   /* 尾调用：lr 始终为 EXC_RETURN（不被 bl 改写），
                                   C 侧以 bx lr 完成真正的异常返回；不占用 r4 这类
                                   callee-saved 寄存器，避免破坏任务的 r4-r11（它们不在
                                   异常帧里，异常返回不会恢复，若被 EXC_RETURN 覆盖会
                                   导致任务寄存器被破坏，如 ok 变 0xFFFFFFFD） */

    .global HardFault_Handler
    .type HardFault_Handler, %function
    .thumb_func
HardFault_Handler:
    tst    lr, #4
    ite    eq
    mrseq  r0, MSP
    mrsne  r0, PSP
    mov    r1, lr           /* arg2 = EXC_RETURN */
    b      rtos_fault_handler   /* 尾调用：lr 始终为 EXC_RETURN，C 侧以 bx lr 完成
                                   真正的异常返回；不占用 r4 这类 callee-saved，
                                   避免破坏任务的 r4-r11（异常返回不恢复它们，若被
                                   EXC_RETURN 覆盖会导致任务寄存器被破坏，如 ok 变
                                   0xFFFFFFFD）。与 MemManage_Handler 同构。 */

    /* UsageFault：INVSTATE/UNDEFINSTR 等在此首次被捕获（未升级为 HardFault），
     * 栈帧干净，便于定位故障 PC。与 HardFault 共用 C 侧处理。 */
    .global UsageFault_Handler
    .type UsageFault_Handler, %function
    .thumb_func
UsageFault_Handler:
    tst    lr, #4
    ite    eq
    mrseq  r0, MSP
    mrsne  r0, PSP
    mov    r1, lr           /* arg2 = EXC_RETURN */
    b      rtos_fault_handler   /* 尾调用（见 HardFault_Handler 注释） */

    /* BusFault：总线错误（如 MPU 区域外对齐访问）同样交 C 侧处理。 */
    .global BusFault_Handler
    .type BusFault_Handler, %function
    .thumb_func
BusFault_Handler:
    tst    lr, #4
    ite    eq
    mrseq  r0, MSP
    mrsne  r0, PSP
    mov    r1, lr           /* arg2 = EXC_RETURN */
    b      rtos_fault_handler   /* 尾调用（见 HardFault_Handler 注释） */
