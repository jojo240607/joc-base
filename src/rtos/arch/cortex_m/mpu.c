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
volatile int      g_robust_fault_active = 0;
volatile uint32_t g_robust_fault_cfsr   = 0;
volatile uint32_t g_fault_pc       = 0;
volatile uint32_t g_fault_mmfar    = 0;
volatile uint32_t g_fault_lr       = 0;
volatile uint32_t g_fault_frame    = 0;   /* 故障异常帧基址（事后用 OpenOCD 翻帧定位 PC） */
volatile uint32_t g_fault_pc_raw   = 0;   /* frame[6] 原始值（不做 FP 帧跳过，便于交叉核对） */
char            g_fault_task_name[24] = {0}; /* 最近一次故障的任务名（仅拷贝，绝不在此处 log） */

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

/* g_running 由调度器维护；此处仅用于每任务栈 region 编程 */
extern task_t *g_running;

/* ---- 每任务栈 region（§6 R3） ----
 * 把 region RTOS_MPU_STACK_REGION 重编程为“当前任务栈”范围（unpriv RW、不可执行）。
 * 该 region 让任务栈不可执行(XN)，属常态安全加固；并作为“每任务独立内存视图”的
 * 一部分——开启 RTOS_MPU_PROTECT_KERNEL_RAM（整块 SRAM 仅特权）时，非特权任务仍能
 * 经本 R4 栈 region 读写自己的栈，但无法触碰内核 .data/.bss/堆/其它任务栈。
 * 注：栈底溢出哨兵此前尝试用 subregion[0] 禁访实现，但会导致任务正常使用的栈底（软件
 * 哨兵魔数区 + 初始帧）被 BusFault 级踩坏，故改为【仅软件哨兵】检测栈溢出（rtos_config.h
 * 与文档已注明），R4 不再禁用任何 subregion。
 * 仅当栈大小是 2 的幂且基址对齐到该大小时才干净生效；否则清空 region（退回软件哨兵，
 * 不误 fault）。在 context.S 切换后由 rtos_arch_apply_task_priv 调用。 */
void rtos_mpu_set_task_stack_region(task_t *t) {
#if RTOS_MPU_PER_TASK_STACK
    MPU->RNR = RTOS_MPU_STACK_REGION;
    if (!t || !t->stack_base || t->stack_size < 32) { MPU->RASR = 0; return; }
    uint32_t base = (uint32_t)t->stack_base;
    uint32_t size = (uint32_t)t->stack_size;
    /* 必须 size 为 2 的幂且 base 对齐到 size，subregion[0] 才恰好等于栈底 guard */
    if (((size & (size - 1u)) != 0) || ((base & (size - 1u)) != 0)) {
        MPU->RASR = 0;                       /* 不满足：退回软件哨兵 */
        return;
    }
    uint32_t size_log2 = 31u - (uint32_t)__builtin_clz(size);   /* log2(size) */
    MPU->RBAR = (base & 0xFFFFFFE0u) | (1u << 4) | (RTOS_MPU_STACK_REGION & 0xFu);
    MPU->RASR = (1u << 0)                       /* ENABLE */
              | (1u << 28)                      /* XN = 1（栈不可执行） */
              | (0b011u << 24)                  /* AP = 双方 RW（任务读写自身栈） */
              | ((size_log2 - 1u) << 1);        /* SIZE = log2 - 1（先不禁用 subregion） */
    __DSB();
#else
    (void)t;
#endif
}

void rtos_mpu_init(void) {
    /* 关 MPU 再配置，避免半配置期间异常 */
    MPU->CTRL = 0;

    /* Region 0: Flash（只读 + 可执行）—— 保护代码不被改写。布局见 memmap.h */
    mpu_set_region(0, MEMMAP_FLASH_BASE,        MEMMAP_FLASH_SIZE_LOG2,        MEMMAP_FLASH_AP,        MEMMAP_FLASH_XN);
    /* Region 1: SRAM。RTOS_MPU_PROTECT_KERNEL_RAM=1 时设为“仅特权 RW”，实现内核
     * RAM 隔离（§6 R2）——非特权任务只能经自己的栈 region(R4) + SVC 门访问内存，
     * 无法直接读写内核 .data/.bss/堆/其它任务栈。默认 0：整块 SRAM 双方 RW，与现行
     * “常态任务保持特权 + 共享 IPC 全局”模型零回归（详见 rtos_config.h 注释）。 */
#if RTOS_MPU_PROTECT_KERNEL_RAM
    mpu_set_region(1, MEMMAP_SRAM_BASE, MEMMAP_SRAM_SIZE_LOG2, 0b001u, MEMMAP_SRAM_XN);
#else
    mpu_set_region(1, MEMMAP_SRAM_BASE, MEMMAP_SRAM_SIZE_LOG2, MEMMAP_SRAM_AP, MEMMAP_SRAM_XN);
#endif
    /* Region 2: 外设（仅特权 RW，不可执行） */
    mpu_set_region(2, MEMMAP_PERIPH_BASE,       MEMMAP_PERIPH_SIZE_LOG2,       MEMMAP_PERIPH_AP,       MEMMAP_PERIPH_XN);
    /* Region 3: Flash BIST 备用扇区（仅特权 RW，不可执行）。
     * 编号高于 Region0，重叠时高编号优先，使 flash 烧录自检的“写闪存”不被 RO 拦截，
     * 其余 Flash 仍为只读（代码保护）。 */
    mpu_set_region(3, MEMMAP_FLASH_BIST_BASE,   MEMMAP_FLASH_BIST_SIZE_LOG2,   MEMMAP_FLASH_BIST_AP,   MEMMAP_FLASH_BIST_XN);

    /* Region 5: CCM（内核对象区：TCB 池 + 任务栈，CPU 专用、DMA 不可达）。
     * 非特权任务运行时要读 g_running(TCB，判断是否需要 SVC 门) 并读写自身栈，
     * 故对 CCM 开放 unpriv RW(XN)。与 R4 每任务栈 region 重叠处属性一致，无冲突。
     * 注意：务必放在 R4 之前的固定区编程，且编号不得与 RTOS_MPU_STACK_REGION(4) 冲突。 */
    mpu_set_region(5, MEMMAP_CCM_BASE,          MEMMAP_CCM_SIZE_LOG2,          MEMMAP_CCM_AP,          MEMMAP_CCM_XN);

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

/* ---- 栈水位（docs/rtos-test-plan.md §6.5） ----
 * 创建任务时把“断点(sp)以下、且跳过栈底哨兵区(前 SENTINEL_WORDS 字)的未使用区域”
 * 全填 0xEEEEEEEE；任务运行时压栈会把这些 0xEE 覆盖成真实数据。测量时从“哨兵区之上”
 * 向高地址数连续 0xEE，即为仍空闲字节（高水位）。栈向低地址增长，故空闲区在底部，
 * 自上而下扫描会在初始异常帧(非 0xEE)处立即终止——必须自下而上扫描（FreeRTOS 同构）。 */
#define STACK_WATERMARK 0xEEEEEEEEu

void rtos_stack_fill_watermark(task_t *t) {
    if (!t || !t->stack_base || !t->sp) return;
    uint32_t *p    = (uint32_t *)t->stack_base + SENTINEL_WORDS; /* 跳过栈底哨兵区 */
    uint32_t *spw  = (uint32_t *)t->sp;     /* 初始帧最低地址；其下为未使用区 */
    while (p < spw) *p++ = STACK_WATERMARK;
}

/* 自下而上数连续 0xEE 的字节数（空闲高水位）。起点跳过栈底哨兵(0xCDCD，非 0xEE)，
 * 否则会在哨兵处立即终止。栈底哨兵区占 16B，最坏情况少算 16B，属保守（不影响溢出判定）。 */
static size_t rtos_stack_free_words(task_t *t) {
    if (!t || !t->stack_base) return 0;
    uint32_t *p    = (uint32_t *)t->stack_base + SENTINEL_WORDS;  /* 跳过哨兵 */
    uint32_t *top  = (uint32_t *)((uint8_t *)t->stack_base + t->stack_size);
    size_t free_words = 0;
    while (p < top) {
        if (*p == STACK_WATERMARK) { free_words++; p++; }
        else break;
    }
    return free_words;
}
size_t rtos_stack_free(task_t *t) {
    return rtos_stack_free_words(t) * sizeof(uint32_t);
}
size_t rtos_stack_used(task_t *t) {
    if (!t || !t->stack_size) return 0;
    return t->stack_size - rtos_stack_free(t);
}

/* ---- 自测专用：在非特权下故意写“仅特权”外设区，触发 MemFault ----
 * 用 naked 函数：无 prologue/epilogue，不改动 PSP；因此故障帧里的 LR 直接指向
 * selftest 调用点的下一条指令，且异常返回时 PSP 已正确落回 selftest 栈帧。
 * 故障处理器只需把 PC 恢复为 LR，即可干净跳过本函数（无需猜指令长度/布局）。 */
#if RTOS_SELFTEST
__attribute__((naked))
static void rtos_mpu_do_violation(void) {
    __asm volatile(
        "ldr r3, [%0]\n"                 /* 非特权下读“仅特权”外设区 -> MemFault；
                                           恢复特权后重执行本条，仅做一次无副作用的读 */
        "bx  lr\n"                       /* 重执行读（已特权）成功后正常返回调用点 */
        : : "r"(MEMMAP_PERIPH_BASE) : "r3", "memory"
    );
}
#endif /* RTOS_SELFTEST */

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

    /* 捕获故障任务名：仅拷贝到全局，绝不在此处调用 log_printf——故障上下文中
     * UART TXE 中断被自身屏蔽，log 会忙等死锁。供 OpenOCD/复位后读取，定位故障任务
     * （见 docs/rtos-test-plan.md §5）。 */
    {
        task_t *fme = rtos_running();
        const char *fn = (fme && fme->name) ? fme->name : "";
        int i;
        for (i = 0; i < (int)sizeof(g_fault_task_name) - 1 && fn[i]; i++)
            g_fault_task_name[i] = fn[i];
        g_fault_task_name[i] = '\0';
    }

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

    /* 鲁棒性自测（RTOSROBUST）：捕获“除零 / 未定义指令”等 UsageFault。
     * 与 MPU 越权恢复同构——把异常返回 PC 改为返回地址(LR)，跳过故障指令本身，
     * 触发故障的任务从下一指令继续执行（系统存活，不进入 WFI 停机）。faulting 任务
     * 在特权态运行，无需改 CONTROL；清除 CFSR 后返回 1 表示已恢复。 */
    if (g_robust_fault_active) {
        g_robust_fault_cfsr = cfsr;
        uint32_t *lr_slot = frame + fo + 5u;
        *pc_slot = *lr_slot;                 /* 跳过故障指令，回到调用者 */
        SCB->CFSR = cfsr;                    /* 写 1 清除故障状态位 */
        __ISB();
        return 1;
    }

    /* 真实故障：记录后停机（避免带损坏状态继续运行） */
    for (;;) { __WFI(); }
}

#if RTOS_SELFTEST
/* ---- SRAM 隔离（§6 R2/R3）隔离子测试 ----
 * 启动一个“对齐栈(RTOS_TASK_STACK)”任务，验证：
 *  (a) 每任务栈 region(R4) 已编程：读回 RBAR/RASR 校验 base/size/AP/XN 正确；
 *  (b) 内核 RAM 隔离(R2)：临时整块 SRAM 仅特权，非特权任务写内核全局(.bss)
 *      应被 MPU 拦截并触发 MemFault，由故障处理器捕获恢复。
 * 两者均通过（R4 正确编程 + R2 写入被拦截）即证明 §6 R2/R3 机制生效。 */
static volatile int      g_mpuram_done, g_mpuram_ok;
static volatile int      g_mpu_ram_r4_ok;
static volatile int      g_mpu_ram_fired;
static uint8_t           g_mpu_kram_scratch;          /* .bss 内核全局，用于 R2 测试 */
RTOS_TASK_STACK(g_mpuram_stack, 1024);

__attribute__((naked))
static void rtos_mpu_do_bad_store(void *addr) {
    __asm volatile (
        "movs r3, #0x55\n"
        "str  r3, [%0]\n"                 /* 非特权写受保护地址 -> MemFault */
        "bx   lr\n"                       /* 被故障处理器跳过（PC=LR） */
        : : "r"(addr) : "r3", "memory"
    );
}

static void mpu_ram_test_task(void *arg) {
    (void)arg;
    /* (a) 每任务栈 region(R4) 已编程：读回 RBAR/RASR 校验 base/size/AP/XN 正确 */
    MPU->RNR = RTOS_MPU_STACK_REGION;
    uint32_t rbar = MPU->RBAR;
    uint32_t rasr = MPU->RASR;
    uint32_t base = (uint32_t)g_running->stack_base;
    /* 注意：RBAR 的 VALID 位(bit4) 是只写位，读回恒为 0，故比较时须屏蔽它；
     * 期望 base = (stack_base & ~0x1F) | region 编号（不含 VALID）。 */
    uint32_t exp_cmp = (base & 0xFFFFFFE0u) | (RTOS_MPU_STACK_REGION & 0xFu);
    uint32_t rb_cmp  = rbar & 0xFFFFFFEFu;     /* 屏蔽读回的 VALID 位 */
    g_mpu_ram_r4_ok = (rasr & 1u)                          /* 已使能 */
                     && (rb_cmp == exp_cmp)                /* base+region 命中当前任务栈 */
                     && (((rasr >> 24) & 0x7u) == 0b011u)  /* AP = 双方 RW（任务读写自身栈） */
                     && (((rasr >> 28) & 1u) == 1u);       /* XN = 1（栈不可执行） */

    /* (b) 内核 RAM 隔离(R2)：临时整块 SRAM 仅特权，写内核全局应 MemFault 并恢复。
     * 本任务栈由 R4(unpriv RW) 覆盖，故写自身栈不受影响；但 .bss 内核全局不在 R4 内，
     * 受 R1(仅特权) 拦截。 */
    MPU->RNR = 1; uint32_t saved_rasr = MPU->RASR;
    MPU->RASR = (1u<<0) | (1u<<28) | (0b001u<<24) | ((MEMMAP_SRAM_SIZE_LOG2 - 1u) << 1);
    __DSB(); __ISB();
    g_mpu_violation = 0; g_mpu_test_active = 1;
    __set_CONTROL(0x3u); __ISB();                 /* 降到非特权 */
    rtos_mpu_do_bad_store((void *)&g_mpu_kram_scratch);  /* 写内核全局 -> fault */
    __set_CONTROL(0x2u); __ISB();
    g_mpu_test_active = 0;
    int fired = g_mpu_violation;
    MPU->RNR = 1; MPU->RASR = saved_rasr; __DSB(); __ISB();   /* 还原整块 SRAM */
    g_mpu_kram_scratch = 0;
    g_mpu_ram_fired = fired;
    g_mpuram_ok   = (g_mpu_ram_r4_ok && fired);
    g_mpuram_done = 1;
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

    /* 4) SRAM 隔离（§6 R2/R3）：启动对齐栈任务，验证 (a) 每任务栈 region(R4) 已正确
     *    编程（base/size/AP/XN）、(b) 临时开启内核 RAM 仅特权后非特权任务写内核全局
     *    被 MPU 拦截并恢复。 */
    {
        g_mpuram_done = 0; g_mpuram_ok = 0;
        rtos_task_create("mpuram", mpu_ram_test_task, (void *)0,
                         (uint8_t)(RTOS_PRIO_MAIN), g_mpuram_stack, sizeof(g_mpuram_stack));
        uint32_t to = 0;
        while (!g_mpuram_done && to < 2000) { rtos_msleep(10); to += 10; }
        if (g_mpuram_done && g_mpuram_ok) {
            log_printf(app_log(), LOG_INFO, "rtos", "[MPU] sram-isolation(R2/R3): PASS\n");
        } else {
            ok = 0;
            log_printf(app_log(), LOG_INFO, "rtos",
                       "[MPU] sram-isolation(R2/R3): FAIL (r4_ok=%d fired=%d)\n",
                       g_mpu_ram_r4_ok, g_mpu_ram_fired);
        }
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[MPU] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* 编译期注册：RTOSALL 会遍历该段依次执行 */
RTOS_SELFTEST_ADD("mpu", rtos_mpu_selftest);
#endif /* RTOS_SELFTEST */
