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

# 是否包含 RTOS 自测 + 板载 BIST。开发/覆盖率构建默认 ON；发布构建设为 OFF，
# 剔除全部自测代码与攻击面（rtos 自测文件、BIST、RTOS* 控制台命令、.rtos_selftests 段）。
option(RTOS_SELFTEST "Build with jOS RTOS self-tests + board BIST (OFF for release)" ON)

# P2-1 调度轨迹 trace：在 rtos_pendsv_switch 记录 (tick,from,to,reason) 环形缓冲，
# 供 RTOSTRACE 命令导出。默认 OFF（零开销、零 RAM）；开发/诊断构建设为 ON。
option(RTOS_SCHED_TRACE "Build with scheduler-switch trace ring buffer (RTOSTRACE cmd)" OFF)

add_executable(${PROJECT_NAME}.elf
    # startup MUST come early so its sections are at the base of .text.
    # When placed last, any growth in .o files before it shifts startup
    # sections, which causes a boot hang on this platform.
    startup/startup_stm32f407xx.s
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
    # arch 层（与底层硬件的唯一边界）：Cortex-M4 实现
    src/rtos/arch/cortex_m/port.c
    src/rtos/arch/cortex_m/context.S
    src/rtos/arch/cortex_m/mpu.c)

  # ---- RTOS 纯自测源码（仅 RTOS_SELFTEST=ON 编译；发布构建剔除，不进固件）----
  if(RTOS_SELFTEST)
    target_sources(${PROJECT_NAME}.elf PRIVATE
      src/rtos/rtos_selftest.c
      src/rtos/rtos_stress.c
      src/rtos/rtos_p4.c
      src/rtos/rtos_usr.c
      src/rtos/rtos_basic.c       # T01-T05 + §2.2 调度锁/临界区/SysTick 唤醒（RTOSBASIC）
      src/rtos/rtos_ipc2.c        # S01/S03/S04/S05 边界与 FromISR（RTOSIPC2）
      src/rtos/rtos_robust.c      # §3.1 异常注入 + §3.2 中断风暴 + §3.3 安全正例（RTOSROBUST）
      src/rtos/rtos_irq.c         # 中断实时性/抗压：实时响应延迟有界 + 高频风暴不崩溃（RTOSIRQ）
      src/rtos/rtos_inv.c         # 优先级反转验证：天花板协议把等锁延迟压到有界（RTOSINV）
      src/rtos/rtos_fuzz.c        # 混沌压力：多优先级随机并发 IPC 不崩溃/不死锁（RTOSFUZZ）
      src/rtos/rtos_timer.c       # §6.3 软件定时器 + 48 天 tick 翻转安全（RTOSTIMER；生产定时器在 core/timer.c）
      src/rtos/rtos_ostest.c      # 对照 docs/ostest.md 补齐的缺口用例（自动进入 RTOSALL）
      src/rtos/rtos_accept.c)     # 硬实时验收+稳定性soak+深度故障注入（RTOSACCEPT；不进 RTOSALL 长链）
  endif()

  target_include_directories(${PROJECT_NAME}.elf PRIVATE
    src/rtos
    src/rtos/include
    src/rtos/core)
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
  target_sources(${PROJECT_NAME}.elf PRIVATE
    src/selftest.c               # 板载集成自测（外设统一 device 接口驱动）
    src/task/task_bist.c)        # BIST 后台任务（prio 24，跑完后阻塞）
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

# ---- Rust 应用层（解耦独立工程：joc-app-rust 独立 cargo build 出 libapp.a）----
# 设计：RTOS 只“链接”外部 libapp.a，不内联 cargo build，不与 Rust 工程耦合源码。
# 使用：
#   cmake -DRUST_APP_LIB=/abs/path/to/joc-app-rust/.../libapp.a ..
# 不指定 RUST_APP_LIB 则纯 C 固件（无 Rust 任务），保持兼容。
# 覆盖率(COVERAGE)下不链接 Rust（避免与 --coverage 冲突）。
# Rust 入口符号(rust_task_entry/rust_ticks/rust_wait_once)经 rust_app.h 引用。
if(RUST_APP_LIB AND NOT COVERAGE)
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
  # 或单独扩展。验收套件 rtos_accept.c 覆盖的正是这些核心路径，已足以量化“测试对内核的覆盖度”。
  set(_cov_srcs
    src/rtos/core/sched.c)
  if(RTOS_SELFTEST)
    # 常态只插桩验收套件 rtos_accept.c（覆盖核心调度/IPC/MPU/故障恢复路径足矣）。
    # 其余纯自测源码默认不插桩（单文件 gcov 计数数组较大，全开 F407 192K SRAM 装不下）；
    # 如需量化“自测代码自身”的覆盖，另开 -DCOVERAGE_SELFTEST=ON（见下方 _cov_st_srcs）。
    list(APPEND _cov_srcs
      src/rtos/rtos_accept.c)
  endif()
  set_source_files_properties(${_cov_srcs} PROPERTIES COMPILE_FLAGS --coverage)
  # 选择性插桩纯自测文件（默认关：单文件 gcov 计数数组较大，全开 F407 192K SRAM 装不下）。
  # 开 -DCOVERAGE_SELFTEST=ON 才额外插桩 rtos_selftest/stress/p4/...，用于量化“自测代码自身”
  # 的覆盖；常态只插桩内核 core + rtos_accept.c（验收套件），足以量化“测试对内核的覆盖度”。
  option(COVERAGE_SELFTEST "Also instrument RTOS arch/osal/analysis/watchdog + pure-selftest sources (needs more RAM)" OFF)
  if(COVERAGE_SELFTEST)
    set(_cov_st_srcs
      src/osal/osal_rtos.c
      src/rtos/core/rtos_sched_analysis.c
      src/rtos/rtos_watchdog.c
      src/rtos/arch/cortex_m/port.c
      src/rtos/arch/cortex_m/mpu.c
      src/rtos/rtos_selftest.c
      src/rtos/rtos_stress.c
      src/rtos/rtos_p4.c
      src/rtos/rtos_usr.c
      src/rtos/rtos_basic.c
      src/rtos/rtos_ipc2.c
      src/rtos/rtos_robust.c
      src/rtos/rtos_irq.c
      src/rtos/rtos_inv.c
      src/rtos/rtos_fuzz.c
      src/rtos/rtos_timer.c
      src/rtos/rtos_ostest.c)
    set_source_files_properties(${_cov_st_srcs} PROPERTIES COMPILE_FLAGS --coverage)
  endif()
endif()

# ---- Post-build: generate .bin / .hex and print size ----
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
    COMMENT "Generating ${PROJECT_NAME}.bin / .hex"
)
