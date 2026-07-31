set pagination off
set print pretty off
file build/stm32f407_minimal.elf
target remote localhost:3333
monitor halt
echo === UART OBJ (real) ===\n
x/8xw (uint32_t)g_app_ctx.uart
echo === PROBE IDX ===\n
x/w &g_rb_probe_idx
echo === UART ADDR ARRAY ===\n
x/16xw &g_rb_uart_addr
echo === UART PROBE ARRAY ===\n
x/16xw &g_rb_uart_probe
echo === SCAN CCM for corrupted LR 0x08000719 ===\n
find /w 0x10000000, 0x10010000, 0x08000719
echo === SCAN CCM for WWDG_IRQHandler addr 0x08000718 ===\n
find /w 0x10000000, 0x10010000, 0x08000718
echo === MSP STACK TAIL (0x1000FC00) ===\n
x/64xw 0x1000FC00
