/*
 * ESP32-C3 (RISC-V RV32IMC) Minimal Startup file
 *
 * Phase-1 Renode 仿真启动：
 *   - .vectors：真实芯片的复位向量表（offset 0 = _start，offset 5 = mtvec 直连
 *     _trap_handler）。Renode 从 ELF entry(_start) 启动，向量表仅作占位/未来
 *     真实芯片使用。
 *   - _start：设 SP（DRAM 顶）、设 GP、清 BSS、拷 .data、调 main，返回后 WFI 自旋。
 *   - 无 FPU 初始化（RV32IMC 无 F 扩展）；无 __libc_init_array 调用（工程为纯 C、
 *     无静态构造器，F103 启动同款做法）。
 */
    .section .vectors, "ax", %progbits
    .align 2
    .word _start                /* 0x00: reset 向量 */
    .word 0                     /* 0x04: 保留 */
    .word 0                     /* 0x08: 保留 */
    .word 0                     /* 0x0C: 保留 */
    .word 0                     /* 0x10: 保留 */
    .word _trap_handler         /* 0x14: mtvec 直连向量（Direct 模式） */

    .section .text
    .global _start
    .type _start, %function
    .align 2
_start:
    /* 栈指针 = DRAM 顶（链接脚本 PROVIDE(__stack_top)） */
    la    sp, __stack_top
    /* 全局指针：小数据基址。注意：必须用 .option norelax 防止链接器将
     * la gp, __global_pointer$ 松弛为 gp-relative (addi gp,gp,offset)
     * ——那会要求 gp 已经被初始化为正确的基址，但此时 gp 可能为零。 */
    .option norelax
    la    gp, __global_pointer$
    .option relax

    /* ---- 清零 BSS ---- */
    la    t0, __bss_start__
    la    t1, __bss_end__
    beq   t0, t1, .L_data_copy
.L_bss_loop:
    sw    zero, 0(t0)
    addi  t0, t0, 4
    bltu  t0, t1, .L_bss_loop

    /* ---- 拷贝 .data（LMA -> VMA）。RAM-only 加载时 __data_load__ 与
     *      __data_start__ 重合，循环体不执行，天然安全 ---- */
.L_data_copy:
    la    t0, __data_load__
    la    t1, __data_start__
    la    t2, __data_end__
    beq   t1, t2, .L_call_main
.L_data_loop:
    lw    t3, 0(t0)
    sw    t3, 0(t1)
    addi  t0, t0, 4
    addi  t1, t1, 4
    bltu  t1, t2, .L_data_loop

.L_call_main:
    call  main

    /* main() 不应返回；保险：WFI 自旋 */
.L_halt:
    wfi
    j     .L_halt
    .size _start, . - _start

    .end
