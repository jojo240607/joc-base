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
}

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

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss.*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

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
