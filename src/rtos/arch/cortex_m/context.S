/*
 * Cortex-M4 上下文切换汇编（arch 层实现，ISA 相关）
 *  - PendSV_Handler：保存/恢复 r4-r11 与 EXC_RETURN，调用 C 侧选下一任务。
 *  - SVC_Handler   ：首次切换到第一个任务（从线程模式 MSP 切到 PSP）。
 *  - MemManage / HardFault_Handler：把异常帧交给 C 侧 rtos_fault_handler。
 *
 * 这些符号覆盖 startup 向量表里的弱符号 PendSV_Handler / SVC_Handler /
 * MemManage_Handler / HardFault_Handler。
 *
 * 本文件是“可移植内核核心”与底层之间的隔离层之一：它只依赖 ARMv7-M 指令集
 * 与 Cortex-M 异常模型，不碰任何芯片外设。
 */
    .syntax unified
    .thumb

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
 *   硬件已在异常入口把 r0-r3,r12,lr,pc,xPSR 压到当前栈（PSP），
 *   这里只需手动保存/恢复 r4-r11 和 EXC_RETURN。
 * ------------------------------------------------------------------------- */
PendSV_Handler:
    mrs    r0, PSP
    stmdb  r0!, {r4-r11}      /* 保存通用寄存器 */
    stmdb  r0!, {lr}          /* 保存 EXC_RETURN（标记当前任务上下文） */
    cpsid  i                  /* 关中断，保护就绪/等待链表不被 ISR 并发改 */
    bl     rtos_pendsv_switch /* r0 = 新任务的 sp（PSP） */
    cpsie  i
    ldmia  r0!, {lr}          /* 恢复新任务的 EXC_RETURN */
    ldmia  r0!, {r4-r11}      /* 恢复新任务的通用寄存器 */
    msr    PSP, r0
    bx     lr                 /* 以 EXC_RETURN 返回到新任务 */

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
    mov    r4, lr           /* 用 callee-saved 保存 EXC_RETURN（bl 会覆盖 lr/r1） */
    bl     rtos_fault_handler
    bx     r4              /* 用保存的 EXC_RETURN 做真正的异常返回 */

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
