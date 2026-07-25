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

    /* 若被切出的任务用过 FPU（EXC_RETURN bit4=0，扩展帧），先保存 S16-S31。
     * 执行该 FP 指令会同时把硬件懒栈的 S0-S15 + FPSCR flush 到本任务栈帧，
     * 使整个 FPU 上下文完整落在本任务栈上。S0-S15/FPSCR 由硬件在异常返回时自动取回。
     * 保存顺序（stmdb 递减）决定帧布局：本指令最先执行 → s16-s31 落在 r4-r11 与
     * EXC_RETURN 之上、紧邻硬件异常帧；整体低→高 = [EXC_RETURN][r4-r11][s16-s31][hw帧]，
     * 与下方恢复路径严格对称（恢复先 r4-r11 再 s16-s31）。 */
#if __FPU_PRESENT
    tst    r14, #0x10
    bne    .Lno_fpu_save     /* bit4=1：基本帧，任务没用过 FPU，跳过 S16-S31 */
    vstmdb r0!, {s16-s31}
.Lno_fpu_save:
#endif

    /* 保存 r4-r11，再单独保存 EXC_RETURN 到【最低地址】。
     * 约定（与 task_stack_init / SVC_Handler 一致）：任务帧低→高 = [EXC_RETURN][r4..r11][(s16-s31)][hw帧]。
     * 注意：不能用 "stmdb r0!, {r4-r11, r14}" —— 该写法会把 r14(EXC_RETURN) 放到最高字，
     * 与初始帧的 EXC_RETURN(word0) 错位，导致首次切换恢复出 r14=0 → INVSTATE。 */
    stmdb  r0!, {r4-r11}
    stmdb  r0!, {r14}        /* EXC_RETURN 在最底部（最低地址） */

    cpsid  i                  /* 关中断，保护就绪/等待链表不被 ISR 并发改 */
    bl     rtos_pendsv_switch /* r0 = 新任务的 sp（PSP） */
    cpsie  i

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
    ldr    r0, =g_running
    ldr    r0, [r0]
    ldr    r1, [r0]           /* first->sp（指向 EXC_RETURN 槽） */
    add    r1, r1, #4         /* 跳过 EXC_RETURN 槽（首次启动不用，PendSV 才用） */
    mov    r2, #2             /* CONTROL: SPSEL=1(用 PSP), nPRIV=0(特权) */
    msr    CONTROL, r2
    isb
    ldmia  r1!, {r4-r11}      /* 弹出 r4-r11 */
    msr    PSP, r1            /* PSP 现在指向异常帧 */
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
    mov    r4, lr           /* 用 callee-saved 保存 EXC_RETURN */
    bl     rtos_fault_handler
    bx     r4              /* 用保存的 EXC_RETURN 做真正的异常返回 */

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
    mov    r1, lr
    mov    r4, lr
    bl     rtos_fault_handler
    bx     r4

    /* BusFault：总线错误（如 MPU 区域外对齐访问）同样交 C 侧处理。 */
    .global BusFault_Handler
    .type BusFault_Handler, %function
    .thumb_func
BusFault_Handler:
    tst    lr, #4
    ite    eq
    mrseq  r0, MSP
    mrsne  r0, PSP
    mov    r1, lr
    mov    r4, lr
    bl     rtos_fault_handler
    bx     r4
