cmake_minimum_required(VERSION 3.20)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake/toolchain.cmake CACHE FILEPATH "" FORCE)

project(stm32f407_minimal C CXX ASM)

# ---- MCU specific ----
# STM32F4 Discovery: STM32F407VGT6, HSE = 8 MHz on board
set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16)
set(MCU_DEFINE "STM32F407xx")
set(HSE_VALUE 8000000)   # Discovery board external crystal = 8 MHz

add_executable(${PROJECT_NAME}.elf
    src/main.c
    src/syscalls.c
    src/system_stm32f4xx.c
    # interface layer (single unified driver interface — header-only, platform-independent)
    # driver layer (implements interfaces, delegates to HAL — platform-independent)
    src/drv/adc.c
    src/drv/gpio_pin.c
    src/drv/clock.c
    src/drv/temp_sensor.c
    src/drv/uart.c
    src/drv/pinmux.c
    # interrupt framework (platform-independent API + callback registry)
    src/irq/irq.c
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
    src/selftest.c
    startup/startup_stm32f407xx.s
)

target_include_directories(${PROJECT_NAME}.elf PRIVATE
    src
    src/device
    src/cmsis
    src/iface
    src/drv
    src/devmgr
    src/board
    src/hal
    src/hal/stm32
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

# ---- Post-build: generate .bin / .hex and print size ----
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
    COMMENT "Generating ${PROJECT_NAME}.bin / .hex"
)
