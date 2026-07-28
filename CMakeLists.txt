cmake_minimum_required(VERSION 3.20)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake/toolchain.cmake CACHE FILEPATH "" FORCE)

project(stm32f407_minimal C CXX ASM)

# ---- MCU specific ----
# STM32F4 Discovery: STM32F407VGT6, HSE = 8 MHz on board
set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16)
set(MCU_DEFINE "STM32F407xx")
set(HSE_VALUE 8000000)   # Discovery board external crystal = 8 MHz

# ---- 覆盖率构建（docs/rtos-test-plan.md §6.6）----
# 开启后加入 --coverage（= -fprofile-arcs -ftest-coverage），并在运行时经调试
# UART 导出 .gcda 帧（见 src/common/gcov_dump.c + 命令 RTOSCOV）；host 端用
# tools/coverage_collect.py 收帧落盘并跑 gcov 出报告。常态关闭，不影响正常构建。
option(COVERAGE "Build with gcov coverage instrumentation (--coverage)" OFF)

add_executable(${PROJECT_NAME}.elf
    # startup MUST come early so its sections are at the base of .text.
    # When placed last, any growth in .o files before it shifts startup
    # sections, which causes a boot hang on this platform.
    startup/startup_stm32f407xx.s
    src/main.c
    src/app_shared.c         # 跨任务共享状态（g_app_ctx 实例 + 心跳/就绪标志）
    src/console.c            # 命令解释器（命令表 + handler，从 main.c 抽出）
    # 应用任务（每个任务一个 c/h，自管栈 + 自带 RTOS_TASK 段注册；见 src/task/）
    src/task/task_app_main.c # main 任务：设备拉起 + 起 BIST + 跑控制台
    src/task/task_blink.c    # blink 演示任务
    src/task/task_idle.c     # 空闲任务
    src/task/task_bist.c     # 板级自测后台任务
    src/task/task_button.c   # 按键中断示例(BH 任务版)：上半部(ISR) -> 下半部(专属 BH 任务)
    src/task/task_button_wq.c # 按键中断示例(工作队列版)：上半部(ISR) -> 下半部(共享 wq 任务，不建新任务)
    src/task/task_template.c # 普通任务模板（复制改名即可新建任务）
    src/system_init.c        # 系统早期初始化（从 main.c 抽出）
    src/syscalls.c
    src/system_stm32f4xx.c
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
    src/drv/dma.c
    # interrupt framework (platform-independent API + callback registry)
    src/irq/irq.c
    src/irq/irq_manager.c   # centralized interrupt manager (registry + dump)
    # device manager (generic name -> device* registry — platform-independent)
    src/devmgr/device_manager.c
    # board layer (per-board: the only place that knows the real peripherals)
    src/board/stm32f4_discovery.c
    # hardware abstraction layer (chip-specific register ops)
    src/hal/stm32/adc_hal.c
    src/hal/stm32/gpio_hal.c
    src/hal/stm32/clock_hal.c
    src/hal/stm32/uart_hal.c
    src/hal/stm32/temp_hal.c
    src/hal/stm32/pinmux_hal.c
    src/hal/stm32/pinmux_af_stm32f4.c
    src/hal/stm32/irq_hal.c
    src/hal/stm32/tim_hal.c
    src/hal/stm32/exti_hal.c
    src/hal/stm32/i2c_hal.c
    src/hal/stm32/spi_hal.c
    src/hal/stm32/sdio_hal.c
    src/hal/stm32/dac_hal.c
    src/hal/stm32/rtc_hal.c
    src/hal/stm32/rng_hal.c
    src/hal/stm32/crc_hal.c
    src/hal/stm32/iwdg_hal.c
    src/hal/stm32/wwdg_hal.c
    src/hal/stm32/flash_hal.c
    src/hal/stm32/i2s_hal.c
    src/hal/stm32/can_hal.c
    src/hal/stm32/usb_hal.c
    src/hal/stm32/dma_hal.c
    # ST USB OTG FS device library (silicon + control engine + CDC class) wrapped in our HAL
    src/hal/stm32/st_usb/usb_core.c
    src/hal/stm32/st_usb/usb_dcd.c
    src/hal/stm32/st_usb/usb_dcd_int.c
    src/hal/stm32/st_usb/usbd_core.c
    src/hal/stm32/st_usb/usbd_req.c
    src/hal/stm32/st_usb/usbd_ioreq.c
    src/hal/stm32/st_usb/usb_bsp.c
    src/hal/stm32/st_usb/usbd_cdc_core.c
    src/selftest.c
)

# ---- RTOS 开关：ON 接入 jOS 调度器（osal_rtos.c + rtos/*），OFF 退回裸机 ----
option(RTOS "Build with jOS RTOS" ON)
if(RTOS)
  target_sources(${PROJECT_NAME}.elf PRIVATE
    src/osal/osal_rtos.c
    # 内核核心（core/ 拆分实现，见 docs/rtos-design.md §10）
    src/rtos/core/sched.c
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
    src/rtos/rtos_selftest.c
    src/rtos/rtos_stress.c
    src/rtos/rtos_p4.c
    src/rtos/rtos_usr.c
    src/rtos/rtos_basic.c       # T01-T05 + §2.2 调度锁/临界区/SysTick 唤醒（RTOSBASIC）
    src/rtos/rtos_ipc2.c        # S01/S03/S04/S05 边界与 FromISR（RTOSIPC2）
    src/rtos/rtos_robust.c      # §3.1 异常注入 + §3.2 中断风暴 + §3.3 安全正例（RTOSROBUST）
    src/rtos/rtos_timer.c       # §6.3 软件定时器 + 48 天 tick 翻转安全（RTOSTIMER）
    src/rtos/rtos_watchdog.c    # §6.4 看门狗喂狗(定时任务驱动) + 马拉松长跑任务组 + 复位原因
    # arch 层（与底层硬件的唯一边界）：Cortex-M4 实现
    src/rtos/arch/cortex_m/port.c
    src/rtos/arch/cortex_m/context.S
    src/rtos/arch/cortex_m/mpu.c)
  target_include_directories(${PROJECT_NAME}.elf PRIVATE
    src/rtos
    src/rtos/include
    src/rtos/core)
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE JOC_RTOS=1)
else()
  target_sources(${PROJECT_NAME}.elf PRIVATE
    src/osal/osal_baremetal.c)
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
    src/hal/stm32
    src/hal/stm32/st_usb
    src/irq
)

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
    ${MCU_DEFINE}
    HSE_VALUE=${HSE_VALUE}UL
)

target_compile_options(${PROJECT_NAME}.elf PRIVATE
    ${CPU_FLAGS}
    -Wall -Wextra -Wno-unused-parameter
    -ffunction-sections -fdata-sections -fstack-usage
    -O2 -g3
    -std=gnu11
)

target_link_options(${PROJECT_NAME}.elf PRIVATE
    ${CPU_FLAGS}
    -T${CMAKE_SOURCE_DIR}/linker/STM32F407VGTX_FLASH.ld
    -specs=nano.specs
    -Wl,-Map=${PROJECT_NAME}.map,--cref
    -Wl,--gc-sections
    -Wl,--no-warn-rwx-segments
    -Wl,--print-memory-usage
)

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
  set_source_files_properties(
    src/osal/osal_rtos.c
    src/rtos/core/sched.c
    src/rtos/core/task.c
    src/rtos/core/timer.c
    src/rtos/core/kobj.c
    src/rtos/core/syscalls.c
    src/rtos/core/bh.c
    src/rtos/core/ipc_sem.c
    src/rtos/core/ipc_mutex.c
    src/rtos/core/ipc_mq.c
    src/rtos/core/ipc_event.c
    src/rtos/core/ipc_bus.c
    src/rtos/rtos_selftest.c
    src/rtos/rtos_stress.c
    src/rtos/rtos_p4.c
    src/rtos/rtos_usr.c
    src/rtos/rtos_basic.c
    src/rtos/rtos_ipc2.c
    src/rtos/rtos_robust.c
    src/rtos/rtos_timer.c
    src/rtos/rtos_watchdog.c
    src/rtos/arch/cortex_m/port.c
    src/rtos/arch/cortex_m/mpu.c
    PROPERTIES COMPILE_FLAGS --coverage)
endif()

# ---- Post-build: generate .bin / .hex and print size ----
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
    COMMENT "Generating ${PROJECT_NAME}.bin / .hex"
)
