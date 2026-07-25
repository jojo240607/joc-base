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
 * 已抽到 arch/cortex_m/memmap.h —— 换芯片只需改那一处头文件。
 * ------------------------------------------------------------------------- */

/* 移植旋钮 + ISA 级最小 IRQn 定义（见 cortex_m.h 注释；换芯片只改该头） */
#include "cortex_m.h"
/* 芯片内存映射（MPU 区域布局）：换芯片只改 memmap.h */
#include "memmap.h"
#include "core_cm4.h"   /* CMSIS ISA 头：SCB / NVIC / FPU / SysTick_IRQn */
#include "mpu_armv7.h"  /* CMSIS ISA 头：MPU_Type / MPU / MPU_CTRL_*（MPU 是 ISA 特性） */

volatile int      g_mpu_violation  = 0;
volatile int      g_mpu_test_active = 0;
volatile int      g_stack_overflow  = 0;
volatile uint32_t g_fault_cfsr     = 0;
volatile uint32_t g_fault_pc       = 0;
volatile uint32_t g_fault_mmfar    = 0;
volatile uint32_t g_fault_lr       = 0;
volatile uint32_t g_fault_frame    = 0;   /* 故障异常帧基址（事后用 OpenOCD 翻帧定位 PC） */
volatile uint32_t g_fault_pc_raw   = 0;   /* frame[6] 原始值（不做 FP 帧跳过，便于交叉核对） */

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

    /* Region 0: Flash（只读 + 可执行）—— 保护代码不被改写。布局见 memmap.h */
    mpu_set_region(0, MEMMAP_FLASH_BASE,        MEMMAP_FLASH_SIZE_LOG2,        MEMMAP_FLASH_AP,        MEMMAP_FLASH_XN);
    /* Region 1: SRAM（RW，不可执行） */
    mpu_set_region(1, MEMMAP_SRAM_BASE,         MEMMAP_SRAM_SIZE_LOG2,         MEMMAP_SRAM_AP,         MEMMAP_SRAM_XN);
    /* Region 2: 外设（仅特权 RW，不可执行） */
    mpu_set_region(2, MEMMAP_PERIPH_BASE,       MEMMAP_PERIPH_SIZE_LOG2,       MEMMAP_PERIPH_AP,       MEMMAP_PERIPH_XN);
    /* Region 3: Flash BIST 备用扇区（仅特权 RW，不可执行）。
     * 编号高于 Region0，重叠时高编号优先，使 flash 烧录自检的“写闪存”不被 RO 拦截，
     * 其余 Flash 仍为只读（代码保护）。 */
    mpu_set_region(3, MEMMAP_FLASH_BIST_BASE,   MEMMAP_FLASH_BIST_SIZE_LOG2,   MEMMAP_FLASH_BIST_AP,   MEMMAP_FLASH_BIST_XN);

    /* 使能 MPU；PRIVDEFENA=1 让特权代码拥有背景区（对现行特权任务透明）。 */
    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    __DSB();
    __ISB();

    /* 打开 MemManage 异常（否则 MPU 违例会升级为 HardFault） */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    /* 同时打开 UsageFault / BusFault：INVSTATE/UNDEFINSTR/对齐错等被首次捕获
     * （不升级为 HardFault），栈帧干净，便于事后定位故障 PC。 */
    SCB->SHCSR |= (SCB_SHCSR_USGFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk);
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
        "ldr r3, [%0]\n"                 /* 非特权下读“仅特权”外设区 -> MemFault；
                                           恢复特权后重执行本条，仅做一次无副作用的读 */
        "bx  lr\n"                       /* 重执行读（已特权）成功后正常返回调用点 */
        : : "r"(MEMMAP_PERIPH_BASE) : "r3", "memory"
    );
}

/* ---- 故障处理（MemManage_Handler 调用） ----
 * 注意：故障处理器运行在异常上下文，UART TX 需要的 TXE 中断被自身屏蔽，
 * 因此这里【禁止调用 log_printf/printf】（会忙等死锁）。只把诊断写入全局，
 * 由 OpenOCD/串口在事后读取；真实故障则停机(WFI)等待调试。 */
int rtos_fault_handler(uint32_t *frame, uint32_t lr) {
    uint32_t cfsr = SCB->CFSR;

    /* 先无条件保存原始信息，再做有风险的 FP 帧跳过解引用（避免二次故障丢失关键数据）。 */
    g_fault_frame  = (uint32_t)frame;
    g_fault_lr     = lr;
    g_fault_cfsr   = cfsr;

    /* 计算基本帧相对异常帧基址的偏移：
     *   EXC_RETURN bit4 = 1 -> 基本帧（无 FPU 懒栈），偏移 0；
     *   EXC_RETURN bit4 = 0 -> 扩展帧（S0-S15+FPSCR 占 0x48 = 0x12 字），偏移 0x12。
     * 硬件异常帧 = [S0..S15, FPSCR] + [R0,R1,R2,R3,R12, LR, PC, xPSR]，
     * 故 LR 在 base+5，PC 在 base+6。PC 解析与“自测恢复”必须共用同一偏移，
     * 否则扩展帧下会把 S5/S6 误当 LR/PC，造成跳到错误地址（曾表现为 IACCVIOL@0x40000000）。 */
    unsigned fo = (lr & 0x10u) ? 0u : 0x12u;
    uint32_t *pc_slot = frame + fo + 6u;

    uint32_t pc = *pc_slot;
    g_fault_pc_raw = pc;
    g_fault_pc     = pc;                     /* 真实故障指令地址（已按帧类型定位） */
    if (cfsr & (1u << 7))                    /* MMFSR.MMARVALID */
        g_fault_mmfar = SCB->MMFAR;
    else
        g_fault_mmfar = 0;

    /* 自测期间的故意越权：捕获后把异常帧的 PC 直接改成“调用点的返回地址”
     * （与 LR 槽相同），使异常返回等价于“从 rtos_mpu_do_violation 正常 bx lr
     * 返回”——干净跳过越权指令本身，无需重执行、无需猜指令长度、与帧类型无关。
     * 帧布局：basic=[R0,R1,R2,R3,R12,LR,PC,xPSR]（fo=0），extended 在其前多 0x12
     * 字 S0-S15+FPSCR（fo=0x12）；LR 恒在 fo+5、PC 恒在 fo+6，而返回所用的
     * EXC_RETURN 正是本入口的 lr，故 fo 由同一个值推导，必然自洽。 */
    if (g_mpu_test_active) {
        g_mpu_violation = 1;
        uint32_t *lr_slot = frame + fo + 5u;
        *pc_slot = *lr_slot;                 /* 异常返回即“函数返回”，跳过越权指令 */
        SCB->CFSR = cfsr;                    /* 写 1 清除故障位 */
        __set_CONTROL(0x2u);                 /* 恢复特权(nPRIV=0)，线程继续用 PSP */
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
