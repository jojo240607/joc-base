/* STM32F407VGTX Flash memory layout (STM32F4 Discovery: STM32F407VGT6)
 * Flash: 1024 KB @ 0x08000000
 * SRAM : 128 KB @ 0x20000000 (SRAM1 112K + SRAM2 16K)
 */
ENTRY(Reset_Handler)

_estack = 0x20020000;        /* end of SRAM (0x20000000 + 128K) */
_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x400;

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
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
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /DISCARD/ :
  {
    *(.note .note.*)
  }
}
