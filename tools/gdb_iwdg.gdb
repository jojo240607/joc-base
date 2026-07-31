file build/stm32f407_minimal.elf
target remote localhost:3333
monitor halt
echo === g_wdt_armed (0x20004c1f) ===\n
x/1xb 0x20004c1f
echo === g_wdt_feed_on / feeds ===\n
x/4xb 0x20004c1e
echo === IWDG registers @0x40003000 (KR/PR/RLR/SR) ===\n
x/4xw 0x40003000
echo === RCC CSR (reset flags) @0x40023874 ===\n
x/1xw 0x40023874
echo === END ===\n
