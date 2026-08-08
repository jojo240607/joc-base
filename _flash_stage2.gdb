
target extended-remote localhost:3333
monitor reset halt
monitor flash write_image erase d:/project/mcu/oop/joc-base/build_stage2/stm32f407_minimal.bin 0x08000000
monitor flash write_image erase d:/project/mcu/oop/joc-base/app.bin 0x08060000
monitor reset halt
detach
quit
