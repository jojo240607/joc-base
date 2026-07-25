#include "rtos_mpu.h"
#include "log/log.h"
#include "log/app_log.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * arch 层（Cortex-M4）：MPU 固定区域编程 + 栈哨兵 + 故障恢复。
 *
 * 本文件是内核核心与底层硬件之间的隔离层：只依赖 ARMv7-M 的 ISA 头
 * core_cm4.h（MPU / SCB），绝不包含任何厂商芯片头（如 stm32f4xx.h）。
 *
 * 注意：MPU 是 ISA 特性（所有 Cortex-M 共有），但“区域布局”（Flash/SRAM/
 * 外设基址与大小）属于芯片内存映射，是换芯片时需要调整的【移植旋钮】，
 * 集中在下方 mpu_set_region() 调用处，便于按新芯片修改。
 * ------------------------------------------------------------------------- */

/* 移植旋钮 + ISA 级最小 IRQn 定义（见 cortex_m.h 注释；换芯片只改该头） */
#include "cortex_m.h"
#include "core_cm4.h"   /* CMSIS ISA 头：SCB / NVIC / FPU / SysTick_IRQn */
#include "mpu_armv7.h"  /* CMSIS ISA 头：MPU_Type / MPU / MPU_CTRL_*（MPU 是 ISA 特性） */

volatile int      g_mpu_violation  = 0;
volatile int      g_mpu_test_active = 0;
volatile int      g_stack_overflow  = 0;
volatile uint32_t g_fault_cfsr     = 0;
volatile uint32_t g_fault_pc       = 0;
volatile uint32_t g_fault_mmfar    = 0;
volatile uint32_t g_fault_lr       = 0;

/* 一个区域：base 必须对齐到 size；ap 见 Cortex-M RASR AP 位；xn=1 禁止执行 */
static void mpu_set_region(uint32_t idx, uint32_t base,
                           uint32_t size_log2, uint32_t ap, int xn) {
    MPU->RNR  = idx;
    MPU->RBAR = (base & 0xFFFFFFE0u) | (1u << 4) | (idx & 0xFu); /* VALID | REGION */
    MPU->RASR = (1u << 0)                       /* ENABLE */
              | ((xn ? 1u : 0u) << 28)          /* XN */
              | ((ap & 0x7u) << 24)             /* AP[2:0] */
              | ((size_log2 - 1u) << 1);        /* SIZE = log2-1 */
}

void rtos_mpu_init(void) {
    /* 关 MPU 再配置，避免半配置期间异常 */
    MPU->CTRL = 0;

    /* Region 0: Flash 0x08000000, 1 MB, RO(双方), 可执行(XN=0) —— 保护代码不被改写 */
    mpu_set_region(0, 0x08000000u, 20, 0b110u, 0);
    /* Region 1: SRAM  0x20000000, 128 KB, RW(双方), 不可执行(XN=1) */
    mpu_set_region(1, 0x20000000u, 17, 0b011u, 1);
    /* Region 2: 外设 0x40000000, 512 MB, 仅特权 RW(0b001), 不可执行 */
    mpu_set_region(2, 0x40000000u, 29, 0b001u, 1);
    /* Region 3: Flash BIST 备用扇区 0x08060000, 128 KB, 仅特权 RW(0b001), 不可执行。
     * 编号高于 Region0，重叠时高编号优先，使 flash 烧录自检的“写闪存”不被 RO 拦截，
     * 其余 Flash 仍为只读（代码保护）。 */
    mpu_set_region(3, 0x08060000u, 17, 0b001u, 1);

    /* 使能 MPU；PRIVDEFENA=1 让特权代码拥有背景区（对现行特权任务透明）。 */
    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    __DSB();
    __ISB();

    /* 打开 MemManage 异常（否则 MPU 违例会升级为 HardFault） */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
}

void rtos_mpu_enable(void)  { MPU->CTRL |=  MPU_CTRL_ENABLE_Msk;  __DSB(); __ISB(); }
void rtos_mpu_disable(void) { MPU->CTRL &= ~MPU_CTRL_ENABLE_Msk;  __DSB(); __ISB(); }

/* ---- 栈哨兵 ---- */
#define STACK_SENTINEL 0xCDCDCDCDu
#define SENTINEL_WORDS 4

void rtos_stack_fill_sentinel(task_t *t) {
    if (!t || !t->stack_base) return;
    uint32_t *p = (uint32_t *)t->stack_base;
    for (uint32_t i = 0; i < SENTINEL_WORDS; i++) p[i] = STACK_SENTINEL;
}
int rtos_stack_check_sentinel(task_t *t) {
    if (!t || !t->stack_base) return 0;
    uint32_t *p = (uint32_t *)t->stack_base;
    for (uint32_t i = 0; i < SENTINEL_WORDS; i++)
        if (p[i] != STACK_SENTINEL) return 1;
    return 0;
}

/* ---- 自测专用：在非特权下故意写“仅特权”外设区，触发 MemFault ----
 * 用 naked 函数：无 prologue/epilogue，不改动 PSP；因此故障帧里的 LR 直接指向
 * selftest 调用点的下一条指令，且异常返回时 PSP 已正确落回 selftest 栈帧。
 * 故障处理器只需把 PC 恢复为 LR，即可干净跳过本函数（无需猜指令长度/布局）。 */
__attribute__((naked))
static void rtos_mpu_do_violation(void) {
    __asm volatile(
        "str %0, [%1]\n"
        "b  .\n"                         /* 理论到不了：str 应已触发 MemFault */
        : : "r"(0xDEADu), "r"(0x40000000u) : "memory"
    );
}

/* ---- 故障处理（MemManage_Handler 调用） ----
 * 注意：故障处理器运行在异常上下文，UART TX 需要的 TXE 中断被自身屏蔽，
 * 因此这里【禁止调用 log_printf/printf】（会忙等死锁）。只把诊断写入全局，
 * 由 OpenOCD/串口在事后读取；真实故障则停机(WFI)等待调试。 */
int rtos_fault_handler(uint32_t *frame, uint32_t lr) {
    uint32_t cfsr = SCB->CFSR;

    /* 计算真正的异常帧：若发生故障时 FPCA=1，硬件会在基本帧前多压 0x48 字节 FP
     * 上下文。EXC_RETURN 的 bit4 = 0 表示压入了 FP 帧，须跳过才能得到 R0..PC。 */
    uint32_t *p = frame;
    if ((lr & (1u << 4)) == 0u) {
        p = (uint32_t *)((uint8_t *)p + 0x48u);
    }

    g_fault_cfsr  = cfsr;
    g_fault_pc    = p[6];
    g_fault_lr    = lr;
    if (cfsr & (1u << 7))                    /* MMFSR.MMARVALID */
        g_fault_mmfar = SCB->MMFAR;
    else
        g_fault_mmfar = 0;

    /* 自测期间的故意越权：标记捕获、清状态、恢复到调用点之后、恢复特权并返回 */
    if (g_mpu_test_active) {
        g_mpu_violation = 1;
        p[6] = p[5];                         /* 故障帧 LR = 调用点下一条指令 */
        SCB->CFSR = cfsr;                    /* 写 1 清除 */
        __set_CONTROL(0x2u);                 /* 返回线程模式，恢复特权(nPRIV=0) */
        __ISB();
        return 1;
    }

    /* 真实故障：记录后停机（避免带损坏状态继续运行） */
    for (;;) { __WFI(); }
}

/* ---- 运行时自测 ---- */
int rtos_mpu_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[MPU] self-test begin\n");

    /* 1) MPU 已启用且区域生效 */
    if (!(MPU->CTRL & MPU_CTRL_ENABLE_Msk)) {
        ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos", "[MPU] enable: FAIL (MPU off)\n");
    } else {
        log_printf(app_log(), LOG_INFO, "rtos", "[MPU] enable: PASS (regions=flash/sram/periph)\n");
    }

    /* 2) 栈哨兵检测机制：故意踩当前任务栈底魔数，验证探测器报溢出，再还原 */
    {
        task_t *me = rtos_running();
        rtos_stack_fill_sentinel(me);
        uint32_t *sb = (uint32_t *)me->stack_base;
        uint32_t saved = sb[0];
        sb[0] = 0xDEADBEEFu;
        int det = rtos_stack_check_sentinel(me);
        sb[0] = saved;                       /* 还原，避免影响后续运行 */
        if (det) log_printf(app_log(), LOG_INFO, "rtos", "[MPU] stack sentinel: PASS\n");
        else { ok = 0; log_printf(app_log(), LOG_INFO, "rtos", "[MPU] stack sentinel: FAIL\n"); }
    }

    /* 3) MPU 越权捕获：临时降到非特权，写受保护的外设区(特权RW)，应触发 MemFault
     *    并被故障处理器捕获恢复（g_mpu_violation 置位）。 */
    g_mpu_violation  = 0;
    g_mpu_test_active = 1;
    __set_CONTROL(0x3u);   /* nPRIV=1(非特权) + SPSEL=1(PSP) */
    __ISB();
    /* 该调用内的 str 在非特权下访问“仅特权”外设区 -> MemFault；
     * 处理器捕获后把 PC 恢复为返回地址，直接回到本调用之后继续。 */
    rtos_mpu_do_violation();
    __set_CONTROL(0x2u);   /* 故障处理器已恢复特权；这里再保险地确保 */
    __ISB();
    g_mpu_test_active = 0;
    if (g_mpu_violation) log_printf(app_log(), LOG_INFO, "rtos", "[MPU] violation caught: PASS\n");
    else { ok = 0; log_printf(app_log(), LOG_INFO, "rtos", "[MPU] violation caught: FAIL\n"); }

    log_printf(app_log(), LOG_INFO, "rtos", "[MPU] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* 编译期注册：RTOSALL 会遍历该段依次执行 */
RTOS_SELFTEST_ADD("mpu", rtos_mpu_selftest);
