cmake_minimum_required(VERSION 3.20)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake/toolchain.cmake CACHE FILEPATH "" FORCE)

set(PROJ_NAME "jOS" CACHE STRING "Project output name")
project(${PROJ_NAME} C CXX ASM)

# Architecture directory (used for RTOS arch layer sources)
set(ARCH_DIR "cortex_m" CACHE STRING "Architecture dir (cortex_m / riscv)")

# ---- Target MCU selection ----
set(JOC_TARGET "STM32F407VGT6" CACHE STRING "Target MCU (STM32F407VGT6 / STM32F103RCT6 / STM32H750VBT6 / ESP32C3)")

if(JOC_TARGET STREQUAL "STM32F103RCT6")
  # STM32F103RCT6: Cortex-M3, 72 MHz, no FPU, no MPU
  set(CPU_FLAGS         -mcpu=cortex-m3 -mthumb -mfloat-abi=soft)
  set(MCU_DEFINE        "STM32F103xx")
  set(CORE_DEFINE       "CORE_CORTEX_M3")
  set(HSE_VALUE         8000000)
  set(BOARD_SRC_FILE    src/board/stm32f103rct6.c)
  set(STARTUP_SRC_FILE  startup/startup_stm32f103xx.s)
  set(SYSTEM_SRC_FILE   system_stm32f1xx.c)
  set(LINKER_TEMPLATE   ${CMAKE_SOURCE_DIR}/linker/STM32F103RCTX_FLASH.ld.in)
  set(HAL_DIR           src/hal/stm32f1)
  # F103 HAL sources (minimal set)
  set(HAL_SRCS
    ${HAL_DIR}/clock_hal.c
    ${HAL_DIR}/gpio_hal.c
    ${HAL_DIR}/uart_hal.c
    ${HAL_DIR}/irq_hal.c
    ${HAL_DIR}/pinmux_hal.c
    ${HAL_DIR}/pinmux_af_stm32f1.c
        ${HAL_DIR}/dma_hal.c
    ${HAL_DIR}/i2c_hal.c
    ${HAL_DIR}/spi_hal.c
    ${HAL_DIR}/tim_hal.c
    ${HAL_DIR}/exti_hal.c
    ${HAL_DIR}/adc_hal.c
    ${HAL_DIR}/iwdg_hal.c
    ${HAL_DIR}/crc_hal.c
    ${HAL_DIR}/wwdg_hal.c
    ${HAL_DIR}/dac_hal.c
    ${HAL_DIR}/flash_hal.c
    ${HAL_DIR}/rtc_hal.c
    ${HAL_DIR}/can_hal.c)
  # F103 driver sources (all available peripherals)
  set(DRV_SRCS
    src/drv/gpio_pin.c
    src/drv/clock.c
    src/drv/uart.c
    src/drv/pinmux.c
    src/drv/systick.c
    src/drv/i2c.c
    src/drv/spi.c
    src/drv/timer.c
    src/drv/pwm.c
    src/drv/exti.c
    src/drv/adc.c
    src/drv/iwdg.c
    src/drv/rtc.c
    src/drv/flash.c
    src/drv/crc.c
    src/drv/dac.c
    src/drv/can.c
    src/drv/dma.c)
  message(STATUS "Target: STM32F103RCT6 (Cortex-M3, ${CPU_FLAGS})")
elseif(JOC_TARGET STREQUAL "STM32F407VGT6")
  # STM32F4 Discovery: STM32F407VGT6, HSE = 8 MHz on board (default)
  set(CPU_FLAGS         -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16)
  set(MCU_DEFINE        "STM32F407xx")
  set(CORE_DEFINE       "CORE_CORTEX_M4")
  set(HSE_VALUE         8000000)
  set(BOARD_SRC_FILE    src/board/stm32f4_discovery.c)
  set(STARTUP_SRC_FILE  startup/startup_stm32f407xx.s)
  set(SYSTEM_SRC_FILE   src/system_stm32f4xx.c)
  set(LINKER_TEMPLATE   ${CMAKE_SOURCE_DIR}/linker/STM32F407VGTX_FLASH.ld.in)
  set(HAL_DIR           src/hal/stm32)
  # F407 HAL sources (full peripheral set)
  set(HAL_SRCS
    ${HAL_DIR}/adc_hal.c
    ${HAL_DIR}/gpio_hal.c
    ${HAL_DIR}/clock_hal.c
    ${HAL_DIR}/uart_hal.c
    ${HAL_DIR}/temp_hal.c
    ${HAL_DIR}/pinmux_hal.c
    ${HAL_DIR}/pinmux_af_stm32f4.c
    ${HAL_DIR}/irq_hal.c
    ${HAL_DIR}/tim_hal.c
    ${HAL_DIR}/exti_hal.c
    ${HAL_DIR}/i2c_hal.c
    ${HAL_DIR}/spi_hal.c
    ${HAL_DIR}/sdio_hal.c
    ${HAL_DIR}/dac_hal.c
    ${HAL_DIR}/rtc_hal.c
    ${HAL_DIR}/rng_hal.c
    ${HAL_DIR}/crc_hal.c
    ${HAL_DIR}/iwdg_hal.c
    ${HAL_DIR}/wwdg_hal.c
    ${HAL_DIR}/flash_hal.c
    ${HAL_DIR}/i2s_hal.c
    ${HAL_DIR}/can_hal.c
    ${HAL_DIR}/usb_hal.c
    ${HAL_DIR}/dma_hal.c
    ${HAL_DIR}/st_usb/usb_core.c
    ${HAL_DIR}/st_usb/usb_dcd.c
    ${HAL_DIR}/st_usb/usb_dcd_int.c
    ${HAL_DIR}/st_usb/usbd_core.c
    ${HAL_DIR}/st_usb/usbd_req.c
    ${HAL_DIR}/st_usb/usbd_ioreq.c
    ${HAL_DIR}/st_usb/usb_bsp.c
    ${HAL_DIR}/st_usb/usbd_cdc_core.c)
  # F407 driver sources (full peripheral set)
  set(DRV_SRCS
    src/drv/adc.c
    src/drv/gpio_pin.c
    src/drv/clock.c
    src/drv/temp_sensor.c
    src/drv/uart.c
    src/drv/pinmux.c
    src/drv/systick.c
    src/drv/timer.c
    src/drv/pwm.c
    src/drv/exti.c
    src/drv/i2c.c
    src/drv/spi.c
    src/drv/sdio.c
    src/drv/sd_card.c
    src/drv/dac.c
    src/drv/rtc.c
    src/drv/rng.c
    src/drv/crc.c
    src/drv/iwdg.c
    src/drv/wwdg.c
    src/drv/flash.c
    src/drv/i2s.c
    src/drv/can.c
    src/drv/usb.c
    src/drv/dma.c)
  message(STATUS "Target: STM32F407VGT6 (Cortex-M4, ${CPU_FLAGS})")
elseif(JOC_TARGET STREQUAL "STM32H750VBT6")
  # STM32H750B-DK: Cortex-M7 @ 480 MHz, double-precision FPU, MPU,
  # 128 KB internal Flash, 128 KB DTCM, 512 KB AXI SRAM, QSPI 16 MB (轨 B).
  set(CPU_FLAGS         -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16)
  set(MCU_DEFINE        "STM32H750xx")
  set(CORE_DEFINE       "CORE_CORTEX_M7")
  set(HSE_VALUE         25000000)
  set(BOARD_SRC_FILE    src/board/stm32h750b_dk.c)
  set(STARTUP_SRC_FILE  startup/startup_stm32h750xx.s)
  set(SYSTEM_SRC_FILE   src/system_stm32h7xx.c)
  set(LINKER_TEMPLATE   ${CMAKE_SOURCE_DIR}/linker/STM32H750VB_FLASH.ld.in)
  set(HAL_DIR           src/hal/stm32h7)
  # H750 HAL sources (milestone 1 minimal set: clock/gpio/uart/irq/pinmux/exti/
  # flash/qspi + tim/dma 链接桩 — drv/timer.c 依赖 tim_hal_* 与 dma_hal_route)
  set(HAL_SRCS
    ${HAL_DIR}/clock_hal.c
    ${HAL_DIR}/gpio_hal.c
    ${HAL_DIR}/uart_hal.c
    ${HAL_DIR}/irq_hal.c
    ${HAL_DIR}/pinmux_hal.c
    ${HAL_DIR}/pinmux_af_stm32h7.c
    ${HAL_DIR}/tim_hal.c
    ${HAL_DIR}/dma_hal.c
    ${HAL_DIR}/exti_hal.c
    ${HAL_DIR}/flash_hal.c
    ${HAL_DIR}/qspi_hal.c
    ${HAL_DIR}/iwdg_hal.c
    ${HAL_DIR}/wwdg_hal.c
    ${HAL_DIR}/crc_hal.c
    ${HAL_DIR}/rng_hal.c
    ${HAL_DIR}/temp_hal.c
    ${HAL_DIR}/adc_hal.c
    ${HAL_DIR}/rtc_hal.c
    ${HAL_DIR}/spi_hal.c
    ${HAL_DIR}/i2c_hal.c
    ${HAL_DIR}/i2s_hal.c
    ${HAL_DIR}/dac_hal.c
    ${HAL_DIR}/sdio_hal.c
    ${HAL_DIR}/usb_hal.c)
  # H750 driver sources (subset matching the milestone-1 HAL surface)
  set(DRV_SRCS
    src/drv/adc.c
    src/drv/gpio_pin.c
    src/drv/clock.c
    src/drv/uart.c
    src/drv/pinmux.c
    src/drv/systick.c
    src/drv/temp_sensor.c
    src/drv/timer.c
    src/drv/dma.c
    src/drv/qspi_flash.c
    src/drv/sdio.c
    src/drv/sd_card.c)
  message(STATUS "Target: STM32H750VBT6 (Cortex-M7, ${CPU_FLAGS})")
elseif(JOC_TARGET STREQUAL "ESP32C3")
  # ESP32-C3: RISC-V RV32IMC @ 40 MHz, no FPU, no MPU
  set(CPU_FLAGS         -march=rv32imc_zicsr_zifencei -mabi=ilp32)
  set(MCU_DEFINE        "ESP32C3")
  set(CORE_DEFINE       "__riscv")
  set(HSE_VALUE         0)
  set(BOARD_SRC_FILE    src/board/esp32c3_devkit.c)
  set(STARTUP_SRC_FILE  startup/startup_esp32c3.s)
  set(LINKER_TEMPLATE   ${CMAKE_SOURCE_DIR}/linker/ESP32C3.ld.in)
  set(HAL_DIR           src/hal/esp32c3)
  set(ARCH_DIR          riscv)
  # ESP32-C3 HAL sources (Phase-1 minimal set)
  set(HAL_SRCS
    ${HAL_DIR}/clock_hal.c
    ${HAL_DIR}/uart_hal.c
    ${HAL_DIR}/irq_hal.c
    ${HAL_DIR}/pinmux_hal.c
    ${HAL_DIR}/dma_hal.c)
  # ESP32-C3 driver sources (Phase-1 minimal set)
  set(DRV_SRCS
    src/drv/clock.c
    src/drv/uart.c)
  message(STATUS "Target: ESP32C3 (RISC-V RV32IMC, ${CPU_FLAGS})")
endif()

# 开启后加入 --coverage（= -fprofile-arcs -ftest-coverage），并在运行时经调试
# UART 导出 .gcda 帧（见 src/common/gcov_dump.c + 命令 RTOSCOV）；host 端用
# tools/coverage_collect.py 收帧落盘并跑 gcov 出报告。常态关闭，不影响正常构建。
option(COVERAGE "Build with gcov coverage instrumentation (--coverage)" OFF)

# 是否包含 RTOS 自测 + 板载 BIST。开发/覆盖率构建默认 ON；发布构建设为 OFF，
# 剔除全部自测代码与攻击面（rtos 自测文件、BIST、RTOS* 控制台命令、.rtos_selftests 段）。
option(RTOS_SELFTEST "Build with jOS RTOS self-tests + board BIST (OFF for release)" ON)

# ---- RTOS 自测按套件裁剪（dev 版 RAM 控制）----
# F407 dev 版(RTOS_SELFTEST=ON)主 SRAM 仅 128KB，全量自测 .bss ~195KB 会 region overflow。
# 按需编译测试条目（未编译的套件由 rtos_selftest.c 弱符号兜底返回 1，console 命令仍可调用，
# 不产生链接错误）。主 .bss 大户：ostest ~29KB / accept(含 1h-soak) ~24KB / basic ~18KB /
# robust ~15.5KB / irq。
# 用法：
#   cmake -DRTOS_SELFTEST=ON -DRTOS_SELFTEST_MINIMAL=ON ..              # F407 lean dev（默认禁重套件）
#   cmake -DRTOS_SELFTEST=ON -DRTOS_SELFTEST_MINIMAL=ON -DRTOS_SELFTEST_ACCEPT=ON ..  # +1h-soak soak 版
# 未显式 -D 时，MINIMAL=ON 会把以下重套件默认置 OFF；用户显式 -DRTOS_SELFTEST_* =ON 可覆盖。
option(RTOS_SELFTEST_MINIMAL "Lean: disable heavy suites by default (F407 dev RAM)" OFF)
if(RTOS_SELFTEST_MINIMAL)
  set(_st_def_ostest OFF)
  set(_st_def_robust  OFF)
  set(_st_def_irq     OFF)
  set(_st_def_accept  OFF)
else()
  set(_st_def_ostest ON)
  set(_st_def_robust  ON)
  set(_st_def_irq     ON)
  set(_st_def_accept  ON)
endif()
set(RTOS_SELFTEST_OSTEST      ${_st_def_ostest} CACHE BOOL "Compile rtos_ostest.c (~29KB .bss, RTOSALL)" )
set(RTOS_SELFTEST_ACCEPT      ${_st_def_accept} CACHE BOOL "Compile stm32 rtos_accept.c (~24KB .bss incl 1h-soak)" )
set(RTOS_SELFTEST_ROBUST      ${_st_def_robust} CACHE BOOL "Compile stm32 rtos_robust.c (~15.5KB .bss)" )
set(RTOS_SELFTEST_IRQ         ${_st_def_irq}    CACHE BOOL "Compile stm32 rtos_irq.c (ISR 实时性/风暴)" )
set(RTOS_SELFTEST_BASIC       ON  CACHE BOOL "Compile rtos_basic.c (T01-T05 + sched-lock/crit/SysTick)" )
set(RTOS_SELFTEST_IPC2        ON  CACHE BOOL "Compile rtos_ipc2.c (S01/S03/S04)" )
set(RTOS_SELFTEST_STRESS      ON  CACHE BOOL "Compile rtos_stress.c" )
set(RTOS_SELFTEST_P4          ON  CACHE BOOL "Compile rtos_p4.c" )
set(RTOS_SELFTEST_USR         ON  CACHE BOOL "Compile rtos_usr.c" )
set(RTOS_SELFTEST_INV         ON  CACHE BOOL "Compile rtos_inv.c" )
set(RTOS_SELFTEST_FUZZ        ON  CACHE BOOL "Compile rtos_fuzz.c" )
set(RTOS_SELFTEST_TIMER       ON  CACHE BOOL "Compile rtos_timer.c" )
set(RTOS_SELFTEST_IPC2S05     ON  CACHE BOOL "Compile stm32 ipc2_s05.c (TIM3 ISR 发信号量)" )
set(RTOS_SELFTEST_OSTEST_ISR  ON  CACHE BOOL "Compile stm32 ostest_isr.c (ISR 用例)" )
set(RTOS_SELFTEST_WATCHDOG_PLAT ON CACHE BOOL "Compile watchdog_plat.c (覆盖弱钩子)" )

# ---- 独立 1h-soak 构建（RTOS_SELFTEST_SOAK）----
# 把 1h 稳定性 soak（acc_b1_soak_long，在 rtos_accept.c）拆到独立非 RTOSALL 构建，
# 避免 dev 版(全量 .bss ~195KB)与 MINIMAL+ACCEPT(~149KB)都溢出 F407 128KB 主 SRAM。
# 开启后仅编译 rtos_accept.c + rtos_selftest.c 框架（其余套件全关，由弱符号兜底），
# 测试 .bss 约 37.6KB，总 RAM ~102KB 可链接。独立构建目录使用，不混入 dev/发布镜像。
# 用法：cmake -S . -B build_f407_soak -G Ninja -DRTOS_SELFTEST_SOAK=ON
option(RTOS_SELFTEST_SOAK "Dedicated 1h-soak build: only rtos_accept.c (+ framework)" OFF)
if(RTOS_SELFTEST_SOAK)
  set(RTOS_SELFTEST          ON  CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_BASIC    OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_IPC2     OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_STRESS   OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_P4       OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_USR      OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_INV      OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_FUZZ     OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_TIMER    OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_OSTEST   OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_ROBUST   OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_IRQ      OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_ACCEPT   ON  CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_IPC2S05  OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_OSTEST_ISR OFF CACHE BOOL "" FORCE)
  set(RTOS_SELFTEST_WATCHDOG_PLAT OFF CACHE BOOL "" FORCE)
  message(STATUS "RTOS_SELFTEST_SOAK=ON: 仅 rtos_accept.c(含 1h-soak)+框架，独立非 RTOSALL 构建")
endif()

# P2-1 调度轨迹 trace：在 rtos_pendsv_switch 记录 (tick,from,to,reason) 环形缓冲，
# 供 RTOSTRACE 命令导出。默认 OFF（零开销、零 RAM）；开发/诊断构建设为 ON。
option(RTOS_SCHED_TRACE "Build with scheduler-switch trace ring buffer (RTOSTRACE cmd)" OFF)

# ---- Renode 模拟器构建开关 ----
# 目标：让固件在 Renode (stm32f4_discovery.repl) 里跑通控制台，供无硬件开发/验证。
# 开启后定义 JOC_RENODE=1，固件侧需规避 Renode 未模拟的外设/握手：
#   - usb0 (USB OTG FS)：Renode 无 OTG 模型，open 会写未映射地址 -> HardFault，跳过；
#   - uart0 控制台：Renode 无 UART<->DMA 握手，DMA TX 会永久阻塞在 TC 信号量，改 IRQ 引擎；
# 其余外设（RCC/GPIO/USART/ADC/TIM/EXTI/I2C/SPI/CAN/iwdg/rtc/rng/crc/flash/DMA 内存搬运）
# Renode 均有模型，行为不变。
option(RENODE "Build for Renode emulation (skip non-emulated peripherals)" OFF)

# ---- App RAM 分区规划（方案 B：开发/发布分流，详见 docs/app-slot-design.md）----
# 主 SRAM 128KB：系统 .data/.bss + 堆放在低端，App 运行期 .bss 下沉到系统堆之下、
# g_app_slot(0x2001DC00, 固定契约) 之上。App RAM 起点/尺寸随构建分流：
#   - 发布版(RTOS_SELFTEST=OFF)：系统 .bss 仅约 14KB，堆留 4KB 后 App RAM 从
#     0x20004800 起到 0x2001DC00，约 101KB（"~105KB" 目标）。
#   - 开发版(RTOS_SELFTEST=ON)：系统 .bss 含大量自测任务栈约 109KB，App RAM 仅留
#     8KB(0x2001C000..0x2001DC00) 保证链接通过；开发版不挂载真 App，空间够用。
# g_app_slot 地址 0x2001DC00 固定不变（跨镜像契约，与 Rust app.ld 一致）。
# CMake 把同一组值经 configure_file 注入链接脚本，并经编译宏注入 app_slot_boot.c，
# 保证系统链接器、系统加载器、Rust app.ld 三处 App RAM 边界严格一致。
if(RTOS_SELFTEST)
  # 开发版系统 .bss 含大量自测任务栈约 118KB，已占满近全部主 SRAM(到 ~0x2001ed1f)，
  # 无任何空间再放静态堆。故开发版静态堆置于 CCMRAM(.ccm_bss, SYS_HEAP_SECTION)：
  # CCM 仅被 RTOS 任务栈/TCB 占 ~56KB/63KB，尚余 ~7KB；驱动对象(usb/usb_hal 等)
  # 为纯软件结构体、不含 DMA 目标缓冲(USB OTG 用外设内部 FIFO)，放 CCM 安全。
  # App RAM 仅作链接占位(开发版不挂载真 App)，须置于 .bss 末端(0x2001ed1f)之上。
  # 主 SRAM 顶部仅余 ~4.7KB，故在此区间依次排布(均需高于 .bss 以避免 VMA 重叠)：
  #   APP_SLOT_RAM @0x2001F000(1KB)  -> g_app_slot(开发版单 ELF 内解析，无需与
  #     独立 app.ld 的 0x2001DC00 契约一致，故可抬离 .bss)
  #   APP_RAM       @0x2001F400(3KB)  -> 开发版主 SRAM 已被 RTOS 自测占满，APP_RAM
  #     区与 .bss 重叠，故 Rust 全局经 .app_data/.app_bss 回归主 SRAM(.bss 后续段，
  #     APP_RAM_MEM=RAM)，APP_RAM 区保持空桩（仅 g_app_loaded 等系统变量占位）
  set(APP_SLOT_ORIGIN "0x2001F000")
  set(APP_RAM_ORIGIN "0x2001F400")
  set(APP_RAM_LENGTH "0xC00")   # 3KB 桩，开发版不加载真 App 到 APP_RAM
  set(APP_RAM_MEM "RAM")        # 开发版 Rust 全局回归主 SRAM（避免与 .bss 重叠）
  set(SYS_STATIC_HEAP_DEF "SYS_STATIC_HEAP")
  # 开发版 CCM 已被 RTOS 自测任务栈/TCB 占满(~60KB ccm_bss)，静态堆改放主 SRAM .bss
  # (7KB 覆盖实测峰测 6124B 设备 malloc 需求)。开发版单 ELF 不链 Rust 飞控(主 SRAM
  # 已 99% 满，含 Rust 8KB 必 region overflow)，飞控验证走轨 B 双分区(app.bin 独立，
  # 不占系统主 SRAM/CCM)。故开发版主 SRAM 余量充裕容纳 7KB 静态堆。
  set(SYS_HEAP_SIZE "0x1C00u")        # 7KB，开发版放主 SRAM .bss，覆盖 6124B 设备需求
  set(SYS_HEAP_SECTION ".bss")
  # 开发版 CCM 已 ~60KB(自测任务栈)逼近 63KB 上限，故可搬 CCM 的 RTOS 管理表
  # 仍留主 SRAM(.bss)，绝不挤占 CCM（避免链接 region overflow）。
  set(RTOS_CCM_DATA_SECTION ".bss")
else()
  # 发布版系统 .bss 实际延伸约到 0x20003608(__bss_end__,见 elf 符号表；g_sys_heap
  # 由 32KB 下调 8KB、且 device_manager/irq/irq_manager/usb_hal/kobj/bh/sched_analysis
  # 等纯软件管理表搬入 CCM 后，主 SRAM .bss 进一步收缩)。
  # 注意：这些管理表现已在 CCM，但 g_app_loaded/g_app_entry 等系统 .bss 仍在主 SRAM
  # (实测 VMA=0x2000343c 附近)，仍需高于 __bss_end__(0x20003608) 隔离。
  # 取 0x20004000(0x20003608 之上留 ~0x9F8 余量,0x1000 对齐)到 g_app_slot(0x2001DC00),
  # 长 0x19C00(约 103KB,较原 67KB 多让出的 ~36KB 给飞控 App)。
  set(APP_SLOT_ORIGIN "0x2001FC00")  # g_app_slot 抬到主 SRAM 顶端 1KB，回收其下死区给 App
  set(APP_RAM_ORIGIN "0x20004000")
  set(APP_RAM_LENGTH "0x1BC00")  # 0x2001FC00 - 0x20004000 = 111KB（回收 g_app_slot 上方 ~9KB 死区；g_app_slot 占顶端 1KB）
  set(APP_RAM_MEM "APP_RAM")     # 发布版 Rust 全局搬进 APP_RAM，释放主 SRAM 系统堆
  set(SYS_STATIC_HEAP_DEF "SYS_STATIC_HEAP")
  set(SYS_HEAP_SIZE "0x2000u")   # 8KB：发布版容纳全部设备 malloc（实测峰测 57 设备累计 6124B，留 ~2KB 余量）
  set(SYS_HEAP_SECTION ".bss")
  # 发布版 CCM 仅用 ~19KB(30%)，余量充足，故把纯软件的 RTOS 管理表/池/注册表
  # (device_manager 节点池/irq 表/kobj/TCB 之外对象/watchdog 栈快照等) 搬入 CCM，
  # 收缩主 SRAM .bss，从而把让出的空间进一步下推给 APP_RAM(飞控 App)。
  # 这些对象均不含 DMA 目标缓冲(CCM 不可 DMA 访问)，放 CCM 安全。
  set(RTOS_CCM_DATA_SECTION ".ccm_bss")
endif()

# ---- STM32F103 RCT6: 48KB SRAM 优化覆盖 ----
# F103 无 CCM 且仅 48KB SRAM，覆盖默认的 F407 布局（128KB SRAM + 64KB CCM）。
if(JOC_TARGET STREQUAL "STM32F103RCT6")
  set(SYS_HEAP_SIZE "0x800u")        # 2KB（RTOS 任务/调度不用 malloc，控制台仅小型解析）
  set(RTOS_CCM_DATA_SECTION ".bss") # 无 CCM -> CCM 数据回退到主 SRAM
  # 双分区（轨 B）：Rust App 独立烧录到 APP_FLASH，RTOS 在 boot 时发现并异步拉起
  set(APP_FLASH_BASE "0x08010000")   # 前 64KB 给 RTOS，之后 192KB 给 App
  set(APP_RAM_ORIGIN "0x20007000")   # RTOS SRAM 缩到 28KB 后，APP_RAM 从 28KB 边界开始
  set(APP_RAM_LENGTH "0x4000")       # 16KB 给 Rust App 运行期 .bss
  set(APP_RAM_MEM "SRAM")
  set(SYS_HEAP_SECTION ".bss")
  # F103 的 RAM 优化：缩减编译期静态池大小
  add_definitions(-DDEVICE_MANAGER_MAX=20)   # 60→20：F103 仅 ~10 设备
  add_definitions(-DRTOS_MAX_TASKS=16)       # 48→16：常驻任务约 6 个
  add_definitions(-DKOBJ_MAX=48)             # 128→48 ：按活跃对象(16任务+~20 IPC)减半
  add_definitions(-DRTOS_BH_MAX=6)           # 8→6 ：F103 外设较少
  add_definitions(-DIRQ_MGR_POOL=20)          # 48→20：F103 IRQ 注册更少
  add_definitions(-DMARATHON_N=2)            # 3→2 ：F103 仅 16 任务上限
  add_definitions(-DMARATHON_STACK_SZ=256)   # 512→256：缓冲减半
  add_definitions(-DRTOS_TIMER_STACK_SIZE=512)  # 1024→512
  add_definitions(-DRTOS_WORKQ_STACK_WORDS=128) # 256→128 字（512 字节栈）
  add_definitions(-DIRQ_MAX_HANDLERS_PER_LINE=2) # 4→2：F103 无共享 IRQ 线
  message(STATUS "F103 RAM layout: 1KB heap, no CCM, 48KB SRAM")
endif()

# ---- STM32H750VBT6: 轨 B 双分区覆盖（DTCM 128K + AXI SRAM 512K + QSPI 16M）----
# RTOS 固件烧内部 Flash(128K)，Rust App 独立烧 QSPI 并经 memory-mapped
# (0x90000000) XIP 执行；App 运行期 .data/.bss 放 AXI SRAM(0x24000000/512K)。
# H7 无 CCM：.ccm_bss/.ccm_heap 由 H750 链接脚本并入 DTCM（仅 CPU 可访问）。
if(JOC_TARGET STREQUAL "STM32H750VBT6")
  set(APP_FLASH_BASE "0x90000000")   # QSPI memory-mapped 区（16MB）
  set(APP_RAM_ORIGIN "0x24000000")   # AXI SRAM 起点
  set(APP_RAM_LENGTH "0x80000")      # 512KB 全量给 App 运行期
  set(APP_RAM_MEM "AXI_SRAM")
  set(SYS_HEAP_SIZE "0x2000u")       # 8KB（SRAM1 非缓存堆，DMA 一致）
  set(SYS_HEAP_SECTION ".sram1_heap")  # H7：堆搬到 SRAM1 (MPU non-cacheable)
  set(RTOS_CCM_DATA_SECTION ".ccm_bss") # 任务栈/TCB 池 -> DTCM
  set(SYS_STATIC_HEAP_DEF "SYS_STATIC_HEAP")
  message(STATUS "H750 轨 B: APP_FLASH=QSPI 0x90000000, APP_RAM=AXI_SRAM 0x24000000/512K")
endif()

# ---- ESP32-C3: 192 KB DRAM 内存分区（Phase-1 最小集）----
if(JOC_TARGET STREQUAL "ESP32C3")
  set(APP_FLASH_BASE "0x40200000")       # flash memory-mapped（app header 探针）
  set(APP_RAM_ORIGIN "0x3FCAF000")       # DRAM 顶 ~0x3FCB0000，留 4KB + 1KB slot
  set(APP_RAM_LENGTH "0x1000")           # 4 KB 桩（Phase-1 无 Rust App）
  set(APP_RAM_MEM "APP_RAM")
  set(APP_SLOT_ORIGIN "0x3FCAFC00")      # 紧接 APP_RAM 之上 1 KB
  set(SYS_HEAP_SIZE "0x800u")            # 2 KB（仅 clock + uart 设备 malloc）
  set(SYS_HEAP_SECTION ".bss")           # 堆放 .bss（无独立堆段）
  set(RTOS_CCM_DATA_SECTION ".bss")      # 无 CCM
  # 不定义 SYS_STATIC_HEAP：用链接器堆（_end .. __HeapLimit）
  message(STATUS "ESP32C3 Phase-1: APP_FLASH=0x40200000, APP_RAM=0x3FCAF000/4K")
endif()

# ---- POST_BUILD RAM 占用统计：主 RAM 区域定义（供 cmake/ram_summary.cmake）----
# 与各 linker 模板 MEMORY 主数据区域一致（F103=SRAM、F407=RAM、H750=DTCM、
# ESP32C3=DRAM）。构建后打印占用率 + OK/WARN/OVERFLOW 状态，便于直接核对。
if(JOC_TARGET STREQUAL "STM32F103RCT6")
  set(RAM_REGION_NAME   "SRAM")
  set(RAM_REGION_ORIGIN "0x20000000")
  set(RAM_REGION_LENGTH 28672)         # 28 KB
elseif(JOC_TARGET STREQUAL "STM32F407VGT6")
  set(RAM_REGION_NAME   "RAM")
  set(RAM_REGION_ORIGIN "0x20000000")
  set(RAM_REGION_LENGTH 131072)        # 128 KB
elseif(JOC_TARGET STREQUAL "STM32H750VBT6")
  set(RAM_REGION_NAME   "DTCM")
  set(RAM_REGION_ORIGIN "0x20000000")
  set(RAM_REGION_LENGTH 131072)        # 128 KB DTCM
elseif(JOC_TARGET STREQUAL "ESP32C3")
  set(RAM_REGION_NAME   "DRAM")
  set(RAM_REGION_ORIGIN "0x3FC80000")
  set(RAM_REGION_LENGTH 196608)        # 192 KB
endif()

# 独立静态系统堆(SYS_STATIC_HEAP，开发版/发布版均启用)：根治原链接器堆
# (开发版 ~440B、发布版 ~4.6KB) 耗尽导致靠后设备(如 usb0 第 54 个)注册失败
# (usb0: NOT REGISTERED)。g_sys_heap 段由 SYS_HEAP_SECTION 决定：
#   - 发布版 .bss(主 SRAM)：落 0x20004e08..0x2000CE08(低于 APP_RAM 0x20006000)
#   - 开发版 .ccm_bss(CCM) ：落 0x1000xxxx，避开已满的主 SRAM
# 原链接器堆路径(_end..__HeapLimit)仅在 SYS_STATIC_HEAP 未定义时作为回退。

# 去掉 SYS_HEAP_SIZE 末尾的 'u'（C 字面量后缀），生成 linker 可用的纯十六进制数值，
# 供链接脚本 .ccm_heap 段尺寸 @SYS_HEAP_SIZE_LD@ 注入使用。必须在 configure_file 之前。
string(REGEX REPLACE "u$" "" SYS_HEAP_SIZE_LD "${SYS_HEAP_SIZE}")

# Configure linker script (substitute @VAR@ placeholders for F407 targets)
get_filename_component(LINKER_LD_NAME ${LINKER_TEMPLATE} NAME_WE)
set(LINKER_OUTPUT ${CMAKE_BINARY_DIR}/linker/${LINKER_LD_NAME}.ld)
configure_file(${LINKER_TEMPLATE} ${LINKER_OUTPUT} @ONLY)

add_executable(${PROJECT_NAME}.elf
    # startup MUST come early so its sections are at the base of .text.
    # When placed last, any growth in .o files before it shifts startup
    # sections, which causes a boot hang on this platform.
    ${STARTUP_SRC_FILE}
    src/main.c
    src/app_shared.c         # 跨任务共享状态（g_app_ctx 实例 + 心跳/就绪标志）
    src/console.c            # 命令解释器（命令表 + handler，从 main.c 抽出）
    # 应用任务（每个任务一个 c/h，自管栈 + 自带 RTOS_TASK 段注册；见 src/task/）
    # 干净默认配置：仅 idle（强制）+ app_main（设备拉起 + 控制台）。
    # 用户层 demo / 飞控示例任务全部迁移到 Rust 应用层（joc-app-rust），
    # 由 Rust 在 rust_app_start() 内经 ABI 契约自行 rtos_task_create 挂上 RTOS。
    src/task/task_app_main.c # main 任务：设备拉起 + 跑控制台（不自动跑 BIST）
    src/task/task_idle.c     # 空闲任务（RTOS 强制最低优先级任务）
    # 以下任务默认不编译（示例/辅助任务）；需要时取消注释即可加回：
    # src/task/task_blink.c    # blink 演示任务（已被 task_demo 的 LED 任务取代）
    # src/task/task_button.c   # 按键中断示例(BH 任务版)
    # src/task/task_button_wq.c # 按键中断示例(工作队列版)
    # src/task/task_template.c # 普通任务模板（复制改名即可新建任务）
    # src/task/task_bist.c     # 板级自测后台任务（RTOS_SELFTEST=OFF 时本就不编译）
    src/system_init.c        # 系统早期初始化（从 main.c 抽出）
    src/syscalls.c
    ${SYSTEM_SRC_FILE}
    # interface layer (single unified driver interface — header-only, platform-independent)
    src/iface/stream_device.c    # stream_device transfer_sync/async default impl
    src/iface/io_xfer.c          # io_xfer_t descriptor + io_xfer_complete (driver/ISR helper)
    # common utility classes (platform-independent, reusable by any business layer)
    src/common/ringbuffer.c      # OOC ring buffer (FIFO) used by stream devices + others
    src/common/gcov_dump.c       # §6.6 gcov .gcda 帧导出（覆盖率构建；常态为空操作）
    src/common/hash.c            # OOC hash table (used by device_manager storage)
    src/common/list.c            # OOC intrusive list (used by device_manager storage)
    src/common/pool.c            # OOC fixed block pool (used by device_manager storage)
    src/bus/bus.c                # lightweight pub/sub event bus (log multi-sink backend)
    src/log/log.c                # OOC logger (level filter + multi-sink)
    src/log/app_log.c            # app glue: logger sunk to console UART
    # OS abstraction layer (semaphore + critical section; RTOS-portable)
    # src/osal: 见下方 RTOS 开关（osal_rtos.c 或 osal_baremetal.c 二选一）
    # driver layer (implements interfaces, delegates to HAL — platform-independent)
    ${DRV_SRCS}
    # interrupt framework (platform-independent API + callback registry)
    src/irq/irq.c
    src/irq/irq_manager.c   # centralized interrupt manager (registry + dump)
    # App 服务表（方案 Y 轻量版）：RTOS 暴露给独立 App 层的函数指针表 + 中断注册位。
    # App 经 extern g_app_slot 引用，只填函数指针/irq_reg[]，不碰裸寄存器/NVIC/VTOR。
    src/app_slot/app_slot.c
    src/app_slot/app_slot_boot.c   # 阶段 2 应用分区自举（从 APP_FLASH 头部发现 App 入口）
    # device manager (generic name -> device* registry — platform-independent)
    src/devmgr/device_manager.c
    # board layer (per-board: the only place that knows the real peripherals)
    ${BOARD_SRC_FILE}
    ${HAL_SRCS}
    # src/selftest.c         # 板载集成自测（RTOS_SELFTEST=OFF 时剔除，见下方条件编译）
)

# ---- RTOS 开关：ON 接入 jOS 调度器（osal_rtos.c + rtos/*），OFF 退回裸机 ----
option(RTOS "Build with jOS RTOS" ON)
if(RTOS)
  target_sources(${PROJECT_NAME}.elf PRIVATE
    src/osal/osal_rtos.c
    # 内核核心（core/ 拆分实现，见 docs/rtos-design.md §10）
    src/rtos/core/sched.c
    src/rtos/core/sched_trace.c  # P2-1 调度轨迹 trace（门控 RTOS_SCHED_TRACE，默认不编译）
    src/rtos/core/rtos_bench.c    # Rhealstone 子集基准 + IRQ→wakeup 延迟直方图（同门控）
    src/rtos/core/task.c
    src/rtos/core/timer.c      # 软件定时器原语 + 48 天 tick 翻转安全（§6.3）
    src/rtos/core/kobj.c
    src/rtos/core/syscalls.c
    src/rtos/core/bh.c
    src/rtos/core/ipc_sem.c
    src/rtos/core/ipc_mutex.c
    src/rtos/core/ipc_mq.c
    src/rtos/core/ipc_event.c
    src/rtos/core/ipc_bus.c
    src/rtos/core/rtos_sched_analysis.c  # 阶段3 可调度性静态自检(WCRT/RTA)
    src/rtos/rtos_watchdog.c    # §6.4 看门狗喂狗(定时任务驱动) + 马拉松长跑任务组 + 复位原因
                                #     （含生产函数 rtos_watchdog_*/rtos_marathon_*，常编；仅末尾
                                #      自测函数 rtos_watchdog_selftest 受 #if RTOS_SELFTEST 门控）
    # arch 层（与底层硬件的唯一边界，由 ARCH_DIR 选 cortex_m / riscv）
    src/rtos/arch/${ARCH_DIR}/port.c
    src/rtos/arch/${ARCH_DIR}/context.S)

  # Cortex-M MPU support (only on ARM targets)
  if(ARCH_DIR STREQUAL "cortex_m")
    target_sources(${PROJECT_NAME}.elf PRIVATE
      src/rtos/arch/cortex_m/mpu.c)
  elseif(ARCH_DIR STREQUAL "riscv")
    # RISC-V stubs (MPU/stack-guard symbols — no-op on RV32IMC)
    target_sources(${PROJECT_NAME}.elf PRIVATE
      src/rtos/arch/riscv/port_stubs.c)
  endif()

  # ---- RTOS 纯自测源码（仅 RTOS_SELFTEST=ON 编译；发布构建剔除，不进固件）----
  # 平台无关用例留在 src/rtos/；依赖芯片外设的用例下沉到 hal/<plat>/test/（见下），
  # RTOS 核心目录不出现平台宏。无平台测试文件的芯片由 rtos_selftest.c 的弱符号兜底。
  if(RTOS_SELFTEST)
    # rtos_selftest.c 常编：含自测运行框架 + 全部测入口弱符号兜底（console 命令依赖，
    # 未编译的套件由弱符号兜底返回 1）。其余套件按 RTOS_SELFTEST_* 开关按需编译
    # （dev 版 RAM 控制：F407 全量 .bss ~195KB 溢出 128KB，MINIMAL 默认裁掉重套件）。
    target_sources(${PROJECT_NAME}.elf PRIVATE
      src/rtos/rtos_selftest.c
    )
    if(RTOS_SELFTEST_BASIC)
      target_sources(${PROJECT_NAME}.elf PRIVATE
        src/rtos/rtos_basic.c   # T01-T05 + §2.2 调度锁/临界区/SysTick 唤醒（RTOSBASIC）
      )
    endif()
    if(RTOS_SELFTEST_IPC2)
      target_sources(${PROJECT_NAME}.elf PRIVATE
        src/rtos/rtos_ipc2.c    # S01/S03/S04 平台无关边界用例（RTOSIPC2；S05 下沉到 hal）
      )
    endif()
    if(RTOS_SELFTEST_STRESS)
      target_sources(${PROJECT_NAME}.elf PRIVATE src/rtos/rtos_stress.c)
    endif()
    if(RTOS_SELFTEST_P4)
      target_sources(${PROJECT_NAME}.elf PRIVATE src/rtos/rtos_p4.c)
    endif()
    if(RTOS_SELFTEST_USR)
      target_sources(${PROJECT_NAME}.elf PRIVATE src/rtos/rtos_usr.c)
    endif()
    if(RTOS_SELFTEST_INV)
      target_sources(${PROJECT_NAME}.elf PRIVATE src/rtos/rtos_inv.c)
    endif()
    if(RTOS_SELFTEST_FUZZ)
      target_sources(${PROJECT_NAME}.elf PRIVATE src/rtos/rtos_fuzz.c)
    endif()
    if(RTOS_SELFTEST_TIMER)
      target_sources(${PROJECT_NAME}.elf PRIVATE
        src/rtos/rtos_timer.c   # §6.3 软件定时器 + 48 天 tick 翻转安全（RTOSTIMER）
      )
    endif()
    if(RTOS_SELFTEST_OSTEST)
      target_sources(${PROJECT_NAME}.elf PRIVATE
        src/rtos/rtos_ostest.c  # 对照 docs/ostest.md 补齐的平台无关缺口用例（自动进入 RTOSALL）
      )
    endif()
    # 平台自测（hal/<plat>/test/）：STM32 家族(F103/F407)用 #ifdef STM32F103xx/F407xx
    # 适配同族芯片；ESP32-C3 提供复位原因 stub 契约；H7 暂无平台测试目录（弱钩子默认通过）。
    if(JOC_TARGET STREQUAL "STM32F103RCT6" OR JOC_TARGET STREQUAL "STM32F407VGT6")
      if(RTOS_SELFTEST_ROBUST)
        target_sources(${PROJECT_NAME}.elf PRIVATE
          src/hal/stm32/test/rtos_robust.c   # §3.1 异常注入 + §3.2 中断风暴 + §3.3 安全正例（RTOSROBUST）
        )
      endif()
      if(RTOS_SELFTEST_IRQ)
        target_sources(${PROJECT_NAME}.elf PRIVATE
          src/hal/stm32/test/rtos_irq.c      # 中断实时性/抗压：实时响应延迟有界 + 高频风暴不崩溃（RTOSIRQ）
        )
      endif()
      if(RTOS_SELFTEST_ACCEPT)
        target_sources(${PROJECT_NAME}.elf PRIVATE
          src/hal/stm32/test/rtos_accept.c   # 硬实时验收+稳定性soak+深度故障注入（RTOSACCEPT；不进 RTOSALL 长链）
        )
      endif()
      if(RTOS_SELFTEST_IPC2S05)
        target_sources(${PROJECT_NAME}.elf PRIVATE
          src/hal/stm32/test/ipc2_s05.c      # S05 从真实 TIM3 溢出中断 ISR 发信号量（RTOSIPC2）
        )
      endif()
      if(RTOS_SELFTEST_OSTEST_ISR)
        target_sources(${PROJECT_NAME}.elf PRIVATE
          src/hal/stm32/test/ostest_isr.c    # TC-Q-004/KERNEL-004/INT-001/003 ISR 用例（ostest_isr）
        )
      endif()
      if(RTOS_SELFTEST_WATCHDOG_PLAT)
        target_sources(${PROJECT_NAME}.elf PRIVATE
          src/hal/stm32/test/watchdog_plat.c # 复位原因解码位组合表（覆盖弱钩子）
        )
      endif()
    elseif(JOC_TARGET STREQUAL "ESP32C3")
      if(RTOS_SELFTEST_WATCHDOG_PLAT)
        target_sources(${PROJECT_NAME}.elf PRIVATE
          src/hal/esp32c3/test/watchdog_plat.c # 复位原因 stub 契约（覆盖弱钩子）
        )
      endif()
    endif()
  endif()

  target_include_directories(${PROJECT_NAME}.elf PRIVATE
    src/rtos
    src/rtos/include
    src/rtos/core)
  target_include_directories(${PROJECT_NAME}.elf PRIVATE
    src/rtos/arch/${ARCH_DIR})
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE JOC_RTOS=1)
  if(RTOS_SCHED_TRACE)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE RTOS_SCHED_TRACE=1)
  endif()
else()
  target_sources(${PROJECT_NAME}.elf PRIVATE
    src/osal/osal_baremetal.c)
endif()

# ---- 板级 BIST + RTOS 自测宏（RTOS_SELFTEST=ON 才编译；发布构建剔除，不进固件）----
# 关闭时定义 RTOS_SELFTEST=0 以覆盖 rtos_config.h 的默认 1，使各 TU 的 #if RTOS_SELFTEST
# 门控与编译期段收集（RTOS_SELFTEST_ADD）no-op 生效，自测代码整体不进入发布镜像。
if(RTOS_SELFTEST)
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE RTOS_SELFTEST=1)
  # F103: skip board-level BIST (F4-only peripheral tests). RTOS self-tests
  # (rtos_accept, rtos_ostest, etc.) are still enabled via RTOS_SELFTEST=1.
  if(NOT JOC_TARGET STREQUAL "STM32F103RCT6" AND NOT JOC_TARGET STREQUAL "STM32H750VBT6" AND NOT JOC_TARGET STREQUAL "ESP32C3")
    target_sources(${PROJECT_NAME}.elf PRIVATE
      src/selftest.c               # 板载集成自测（外设统一 device 接口驱动）
      src/task/task_bist.c)        # BIST 后台任务（prio 24，跑完后阻塞）
  endif()
else()
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE RTOS_SELFTEST=0)
endif()

target_include_directories(${PROJECT_NAME}.elf PRIVATE
    src
    src/device
    src/cmsis
    src/iface
    src/osal
    src/drv
    src/devmgr
    src/board
    src/hal
    ${HAL_DIR}
    src/hal/stm32
    src/hal/stm32/st_usb
    src/irq
    tools/abi                # RTOS<->App ABI 契约头（rtos_abi.h），系统侧 App 服务表引用
)

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
    ${CORE_DEFINE}
    ${MCU_DEFINE}
    HSE_VALUE=${HSE_VALUE}UL
    APP_RAM_BASE=${APP_RAM_ORIGIN}
    APP_RAM_SIZE=${APP_RAM_LENGTH}
    SYS_HEAP_SIZE=${SYS_HEAP_SIZE}
    SYS_HEAP_SECTION=\"${SYS_HEAP_SECTION}\"
    RTOS_CCM_DATA_SECTION=\"${RTOS_CCM_DATA_SECTION}\"
    ${SYS_STATIC_HEAP_DEF}
)

if(JOC_TARGET STREQUAL "STM32F103RCT6")
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
    APP_FLASH_BASE=${APP_FLASH_BASE}
    RTOS_CPU_HZ=72000000UL
    RTOS_USE_MPU=0
    RTOS_SCHED_TRACE=0
    RTOS_CRIT_MAX_CYCLES=144000U
    __FPU_PRESENT=0U
    __MPU_PRESENT=0U
    __DWT_PRESENT=0U)
elseif(JOC_TARGET STREQUAL "STM32F407VGT6")
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
    __FPU_PRESENT=1U
    __MPU_PRESENT=1U
    __DWT_PRESENT=1U)
elseif(JOC_TARGET STREQUAL "STM32H750VBT6")
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
    APP_FLASH_BASE=${APP_FLASH_BASE}
    APP_FLASH_SIZE=0x1000000u          # QSPI 16MB（app_slot.h 默认 0x60000u 被覆盖）
    RTOS_CPU_HZ=480000000UL
    RTOS_USE_MPU=1                     # 里程碑 2：使能 MPU + D-Cache（SRAM1 非缓存堆）
    RTOS_SCHED_TRACE=0
    RTOS_CRIT_MAX_CYCLES=960000U
    __FPU_PRESENT=1U
    __MPU_PRESENT=1U
    __ICACHE_PRESENT=1U
    __DCACHE_PRESENT=1U
    __DWT_PRESENT=0U)
elseif(JOC_TARGET STREQUAL "ESP32C3")
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
    APP_FLASH_BASE=${APP_FLASH_BASE}
    RTOS_CPU_HZ=40000000UL
    RTOS_USE_MPU=0
    RTOS_SCHED_TRACE=0
    RTOS_CRIT_MAX_CYCLES=80000U
    RTOS_MAX_ZERO_LATENCY_IRQS=0
    __FPU_PRESENT=0U)
endif()

# Renode 构建：注入 JOC_RENODE=1，驱动/任务层据此规避未模拟外设。
if(RENODE)
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE JOC_RENODE=1)
  message(STATUS "Renode build ENABLED (JOC_RENODE=1: skip usb0, uart0 IRQ engine)")
endif()

target_compile_options(${PROJECT_NAME}.elf PRIVATE
    ${CPU_FLAGS}
    -Wall -Wextra -Wno-unused-parameter
    -ffunction-sections -fdata-sections -fstack-usage
    -O2 -g3
    -std=gnu11
)

target_link_options(${PROJECT_NAME}.elf PRIVATE
    ${CPU_FLAGS}
    -T${LINKER_OUTPUT}
    $<IF:$<STREQUAL:${JOC_TARGET},ESP32C3>,, -specs=nano.specs>
    -Wl,-Map=${PROJECT_NAME}.map,--cref
    -Wl,--gc-sections
    -Wl,--no-warn-rwx-segments
    -Wl,--print-memory-usage
    $<IF:$<STREQUAL:${JOC_TARGET},ESP32C3>,-nostartfiles,>
)

# ---- Rust 应用层（解耦独立工程：joc-app-rust 独立 cargo build 出 libapp.a）----
# 设计：RTOS 只"链接"外部 libapp.a，不内联 cargo build，不与 Rust 工程耦合源码。
# 使用：
#   cmake -DRUST_APP_LIB=/abs/path/to/joc-app-rust/.../libapp.a ..
# 不指定 RUST_APP_LIB 则纯 C 固件（无 Rust 任务），保持兼容。
# 覆盖率(COVERAGE)下不链接 Rust（避免与 --coverage 冲突）。
# 开发版(RTOS_SELFTEST=ON)主 SRAM 已 99% 满，含 Rust 8KB App 必 region overflow，
# 故开发版强制跳过 Rust 链接（飞控验证走轨 B 双分区，不占系统主 SRAM/CCM）。
# Rust 入口符号(rust_task_entry/rust_ticks/rust_wait_once)经 rust_app.h 引用。
if(RUST_APP_LIB AND NOT COVERAGE AND NOT RTOS_SELFTEST)
  if(NOT EXISTS "${RUST_APP_LIB}")
    message(FATAL_ERROR "RUST_APP_LIB 指向的文件不存在: ${RUST_APP_LIB}")
  endif()
  # IMPORTED 静态库，把 libapp.a 纳入链接（--gc-sections 会裁掉未引用段）。
  add_library(app_rust STATIC IMPORTED)
  set_target_properties(app_rust PROPERTIES IMPORTED_LOCATION "${RUST_APP_LIB}")
  target_link_libraries(${PROJECT_NAME}.elf PRIVATE app_rust)
  target_include_directories(${PROJECT_NAME}.elf PRIVATE ${CMAKE_SOURCE_DIR}/app/rust)
  # 注入 C 宏 RUST_APP_LIB，使 task_app_main.c 的 #ifdef RUST_APP_LIB 分支生效
  # （仅声明/调用 rust_app_start，符号由 libapp.a 提供）。
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE RUST_APP_LIB=1)
  message(STATUS "Rust app layer LINKED from: ${RUST_APP_LIB}")
else()
  message(STATUS "Rust app layer DISABLED (set -DRUST_APP_LIB=<path/libapp.a> to enable)")
endif()

# ---- 覆盖率 ----
# RTOS_COVERAGE 全目标定义（仅影响预处理，几乎不占 RAM）；--coverage 只加在
# RTOS 源文件上：F407 仅 192KB SRAM，全量插桩的计数数组会撑爆主 RAM，而 §6.6
# 的覆盖目标本身就是 RTOS 内核（调度/IPC/定时器/自测），故只插桩 src/rtos/**。
# 链接期 --coverage 负责拉入 libgcov（被插桩对象引用）。
if(COVERAGE)
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE RTOS_COVERAGE=1)
  target_link_options(${PROJECT_NAME}.elf PRIVATE --coverage)
  message(STATUS "Coverage build ENABLED (RTOS-only --coverage + RTOS_COVERAGE)")
endif()

if(COVERAGE AND RTOS)
  # 仅插桩内核核心（调度 + IPC + 任务/定时器/对象/系统调用/BH）。
  # arch 层(port/mpu)、osal、rtos_sched_analysis、rtos_watchdog 暂不在默认覆盖集内——
  # 单文件 gcov 计数数组较大，F407 主 RAM 128KB 装不下全量；如需可加 -DCOVERAGE_SELFTEST
  # 或单独扩展。验收套件 rtos_accept.c 覆盖的正是这些核心路径，已足以量化"测试对内核的覆盖度"。
  set(_cov_srcs
    src/rtos/core/sched.c)
  if(RTOS_SELFTEST)
    # 常态只插桩验收套件 rtos_accept.c（覆盖核心调度/IPC/MPU/故障恢复路径足矣）。
    # 其余纯自测源码默认不插桩（单文件 gcov 计数数组较大，全开 F407 192K SRAM 装不下）；
    # 如需量化"自测代码自身"的覆盖，另开 -DCOVERAGE_SELFTEST=ON（见下方 _cov_st_srcs）。
    if(RTOS_SELFTEST_ACCEPT)
      list(APPEND _cov_srcs
        src/hal/stm32/test/rtos_accept.c)
    endif()
  endif()
  set_source_files_properties(${_cov_srcs} PROPERTIES COMPILE_FLAGS --coverage)
  # 选择性插桩纯自测文件（默认关：单文件 gcov 计数数组较大，全开 F407 192K SRAM 装不下）。
  # 开 -DCOVERAGE_SELFTEST=ON 才额外插桩 rtos_selftest/stress/p4/...，用于量化"自测代码自身"
  # 的覆盖；常态只插桩内核 core + rtos_accept.c（验收套件），足以量化"测试对内核的覆盖度"。
  option(COVERAGE_SELFTEST "Also instrument RTOS arch/osal/analysis/watchdog + pure-selftest sources (needs more RAM)" OFF)
  if(COVERAGE_SELFTEST)
    set(_cov_st_srcs
      src/osal/osal_rtos.c
      src/rtos/core/rtos_sched_analysis.c
      src/rtos/rtos_watchdog.c
      src/rtos/arch/cortex_m/port.c
      src/rtos/arch/cortex_m/mpu.c
      src/rtos/rtos_selftest.c)
    if(RTOS_SELFTEST_STRESS)
      list(APPEND _cov_st_srcs src/rtos/rtos_stress.c)
    endif()
    if(RTOS_SELFTEST_P4)
      list(APPEND _cov_st_srcs src/rtos/rtos_p4.c)
    endif()
    if(RTOS_SELFTEST_USR)
      list(APPEND _cov_st_srcs src/rtos/rtos_usr.c)
    endif()
    if(RTOS_SELFTEST_BASIC)
      list(APPEND _cov_st_srcs src/rtos/rtos_basic.c)
    endif()
    if(RTOS_SELFTEST_IPC2)
      list(APPEND _cov_st_srcs src/rtos/rtos_ipc2.c)
    endif()
    if(RTOS_SELFTEST_ROBUST)
      list(APPEND _cov_st_srcs src/hal/stm32/test/rtos_robust.c)
    endif()
    if(RTOS_SELFTEST_IRQ)
      list(APPEND _cov_st_srcs src/hal/stm32/test/rtos_irq.c)
    endif()
    if(RTOS_SELFTEST_INV)
      list(APPEND _cov_st_srcs src/rtos/rtos_inv.c)
    endif()
    if(RTOS_SELFTEST_FUZZ)
      list(APPEND _cov_st_srcs src/rtos/rtos_fuzz.c)
    endif()
    if(RTOS_SELFTEST_TIMER)
      list(APPEND _cov_st_srcs src/rtos/rtos_timer.c)
    endif()
    if(RTOS_SELFTEST_OSTEST)
      list(APPEND _cov_st_srcs src/rtos/rtos_ostest.c)
    endif()
    set_source_files_properties(${_cov_st_srcs} PROPERTIES COMPILE_FLAGS --coverage)
  endif()
endif()

# ---- Post-build: generate .bin / .hex, print size, and log main-RAM usage ----
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
    COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${PROJECT_NAME}.elf>
        -DSIZE_TOOL=${CMAKE_SIZE}
        -DREGION_NAME=${RAM_REGION_NAME}
        -DREGION_ORIGIN=${RAM_REGION_ORIGIN}
        -DREGION_LENGTH=${RAM_REGION_LENGTH}
        -P ${CMAKE_SOURCE_DIR}/cmake/ram_summary.cmake
    COMMENT "Generating ${PROJECT_NAME}.bin / .hex"
)
