file build/stm32f407_minimal.elf
target remote localhost:3333
monitor halt
echo === pc/lr ===\n
info registers pc lr
echo === g_app_ctx[0] ===\n
x/4xw 0x20000000
echo === uart obj @0x20015670 ===\n
x/64xw 0x20015670
echo === USART1 @0x40011000 (SR/DR/CR) ===\n
x/16xw 0x40011000
echo === DMA2 streams (UART1_RX is DMA2) @0x40026400 ===\n
x/64xw 0x40026400
echo === END ===\n
