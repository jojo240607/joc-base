/* STM32F407VGTX Flash memory layout (STM32F4 Discovery: STM32F407VGT6)
 * Flash: 1024 KB @ 0x08000000
 * SRAM : 128 KB @ 0x20000000 (SRAM1 112K + SRAM2 16K)
 */
ENTRY(Reset_Handler)

/* 主栈(MSP)置于 CCM 顶部：CCM 是 CPU 专用内存(0x10000000, 64K)，
 * DMA 访问不到，正好给“纯 CPU、不与 DMA 交互”的主栈用；主 SRAM 让给
 * UART/USB/SD 等 DMA 缓冲。CCM 顶部 _Min_Stack_Size(1K) 预留给 MSP。 */
_estack = 0x10010000;        /* = CCM base 0x10000000 + 64K */
_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x400;

MEMORY
{
  FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 128K        /* 主 SRAM：.data/.bss/堆 + 所有 DMA 缓冲 */
  CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = 63K         /* 内核对象(任务栈/TCB池)；顶部 1K 留给 MSP */
  /* 阶段 2 应用分区（方案 Y 轻量版独立 App 烧录区）：
   * - APP_FLASH：App 的 .text/.rodata 烧录区，从 sector 7 (0x08060000) 起。
   *   F407 1MB Flash = sector0~11；系统固件(sector0~6 = 64+64+128+128+128=512KB)
   *   之后留 512KB(sector7~11) 给 App，足够独立应用镜像。
   * - APP_RAM：App 运行期 .data/.bss 专用 RAM（固定 8KB，在系统堆之上、SRAM 顶端之下）。
   *   与系统 RAM 布局物理隔离，系统升级不波及 App 内存，App 重烧不波及系统堆。 */
  APP_FLASH (rx) : ORIGIN = 0x08060000, LENGTH = 384K      /* sector 7(128K)+8(128K)+9(128K)，留 sector10/11 冗余 */
  APP_RAM   (xrw): ORIGIN = 0x2001E000, LENGTH = 8K        /* 主 SRAM 顶 8KB：0x2001E000..0x20020000 */
  /* App 服务表 g_app_slot 固定 RAM 地址（方案 Y 解耦关键）：App 独立镜像经
   * PROVIDE(g_app_slot=固定地址) 引用同一地址，故系统升级/App 重烧都不改变该地址。
   * 放在 APP_RAM 之下、系统堆之上，紧邻不重叠。 */
  APP_SLOT_RAM (xrw): ORIGIN = 0x2001DC00, LENGTH = 1K     /* g_app_slot 实例固定落此，约 100B 足够 */
}

/* 堆上界：系统堆止于 APP_SLOT_RAM 起点(0x2001DC00)。主栈在 CCM，故主
 * SRAM[0x20000000..0x2001DC00) 除 .data/.bss 外全部可作堆。APP_SLOT_RAM 与
 * APP_RAM 由 App 加载器/服务表使用，系统不碰、不挪动（地址固定契约）。 */
__HeapLimit = ORIGIN(APP_SLOT_RAM);

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text.*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(16);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata.*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } >FLASH

  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* 编译期收集的自测注册表：每个 RTOS_SELFTEST_ADD 把一项放进 .rtos_selftests.<name>
   * 段；运行时 rtos_selftest_run_all() 遍历 __rtos_selftest_start.._end 依次执行。
   * SORT 保证按名字稳定排序（ipc < mpu < stress ...）。 */
  .rtos_selftests :
  {
    PROVIDE_HIDDEN (__rtos_selftest_start = .);
    KEEP (*(SORT(.rtos_selftests.*)))
    PROVIDE_HIDDEN (__rtos_selftest_end = .);
  } >FLASH

  /* 编译期段收集（P4，见 docs/rtos-design.md 第 5 章）：RTOS_TASK / RTOS_MSGQ /
   * RTOS_BH 宏分别把对象定义放进 ._rtos_tasks / ._rtos_ipc / ._rtos_bh 段；
   * rtos_start() 遍历这些段自动实例化。KEEP 防止被 GC；SORT 稳定排序。 */
  ._rtos_tasks :
  {
    PROVIDE_HIDDEN (__rtos_tasks_start = .);
    KEEP (*(SORT(._rtos_tasks.*)))
    PROVIDE_HIDDEN (__rtos_tasks_end = .);
  } >FLASH
  ._rtos_ipc :
  {
    PROVIDE_HIDDEN (__rtos_ipc_start = .);
    KEEP (*(SORT(._rtos_ipc.*)))
    PROVIDE_HIDDEN (__rtos_ipc_end = .);
  } >FLASH
  ._rtos_bh :
  {
    PROVIDE_HIDDEN (__rtos_bh_start = .);
    KEEP (*(SORT(._rtos_bh.*)))
    PROVIDE_HIDDEN (__rtos_bh_end = .);
  } >FLASH

  /* ===========================================================================
   * 阶段 2 应用分区（方案 Y 轻量版：独立 App 烧录区）。
   * 这些段仅当构建"双分区部署镜像"时才被填充（Rust 侧用独立 app.ld 把
   * libapp.a 的段收集进 app_header/app_code；常态单 ELF 构建里这些输入段
   * 为空，KEEP 为空、布局不占空间，与开发期单 ELF 完全兼容）。
   * 固定地址契约（RTOS 启动加载器 app_slot_boot.c 按此地址寻址）：
   *   - app_header  MUST 位于 APP_FLASH 起点 (0x08060000)
   *   - app_code    紧跟 header 之后（LMA 连续，运行时直接在 XIP Flash 执行）
   * App 的 .data/.bss 进下方 APP_RAM（见 app_bss 段），由加载器清零。
   * =========================================================================== */
  .app_header :
  {
    . = ALIGN(4);
    KEEP(*(.app_header))
    KEEP(*(.app_header.*))
    . = ALIGN(16);
  } >APP_FLASH

  .app_code :
  {
    . = ALIGN(4);
    KEEP(*(.app_text))
    KEEP(*(.app_text.*))
    KEEP(*(.app_rodata))
    KEEP(*(.app_rodata.*))
    . = ALIGN(4);
  } >APP_FLASH

  /* Pad total FLASH footprint to 16-byte boundary (ART prefetch requires
   * 128-bit row alignment — without this, non-16-byte-aligned firmware
   * sizes hang at boot due to prefetch fetching partial rows). */
  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data.*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  /* §6.6 覆盖率构建：gcov 计数/信息段的处理（仅 -DCOVERAGE=ON 时非空；常态构建为空）。
   * 历史教训：
   * 1) 一度放进 CCMRAM —— CCM 仅 63K 已饱和，且 CCM 无默认 MPU 区，unpriv 任务访问即 BusFault。
   * 2) 曾放独立 .gcov 段在 .bss 之前 —— Reset_Handler 只清零 _sbss.._ebss（.bss 段），
   *    漏掉 .gcov 段，gcov 计数器保留 RAM 上电垃圾值，插桩 ++ / __gcov_dump 解引用时卡死
   *    （这正是 P0-2 coverage 构建 BIST 后卡死的根因，旧注释"必须放在 .bss 之前"逻辑反了）。
   * 现行方案：.bss.__gcov0* 计数器直接并入下方 .bss 段（KEEP 防 GC），由 Reset_Handler 的
   * .bss 清零循环统一清零到 0；.data.__gcov_* 自然落入上方 .data 段由 FLASH 拷贝初始化。
   * 两者均落在主 SRAM（>RAM），CCM 腾出空间保持 8K 主栈。 */
  /* gcov 覆盖率计数段(.bss.__gcov0.*)。gcov 计数器必须在上电时清零为 0，
   * 否则插桩 ++ 会在垃圾值上累加、__gcov_dump 解引用时卡死/崩溃。
   * 关键：直接并入 .bss 段（用 KEEP 防 GC），由 Reset_Handler 的 .bss 清零
   * 循环统一清零到 0。早期版本把它放在独立 .gcov 段(.bss 之前)导致清零循环
   * 漏掉、计数器保留 RAM 垃圾值，正是 P0-2 coverage 构建 BIST 后卡死的根因。
   * 仅 -DCOVERAGE=ON 时这些输入段非空；常态构建 KEEP 为空，布局不变。 */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss.*)
    KEEP(*(.bss.__gcov0*))
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* App 服务表（方案 Y 轻量版）：g_app_slot 实例落固定 RAM（APP_SLOT_RAM）。
   * 系统在 app_slot_init() 里运行时填充函数指针 + 头部版本。App 独立镜像经
   * PROVIDE(g_app_slot=0x2001DC00) 引用同一地址，只填 irq_reg[] 注册位，
   * 不碰裸寄存器/NVIC/VTOR。该地址固定不变，系统升级/App 重烧都无需重编 App。
   * KEEP 防止被 GC（g_app_slot 仅由 App 入口 + irq_manager 引用，属跨镜像符号）。 */
  .app_slot (NOLOAD) :
  {
    . = ALIGN(4);
    KEEP(*(.app_slot))
    KEEP(*(.app_slot.*))
    . = ALIGN(4);
  } >APP_SLOT_RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = ALIGN(8);
  } >RAM

  /* CCM BSS：RTOS 任务栈 + TCB 池等“仅 CPU 访问、永不被 DMA 触碰”的对象。
   * 这些 static 数组本在 .bss，显式搬进 CCM 释放主 SRAM 给 DMA 缓冲。
   * 由 RTOS_TASK_STACK 宏 与 g_task_pool 的 section(".ccm_bss") 属性收集。
   * 启动代码(Reset_Handler)会按 __ccm_bss_start/end 将其清零。 */
  .ccm_bss (NOLOAD) :
  {
    . = ALIGN(4);
    __ccm_bss_start = .;
    KEEP(*(.ccm_bss))
    KEEP(*(.ccm_bss.*))
    . = ALIGN(4);
    __ccm_bss_end = .;
  } >CCMRAM

  /DISCARD/ :
  {
    *(.note .note.*)
  }
}
