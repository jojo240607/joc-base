file build/stm32f407_minimal.elf
target remote localhost:3333
monitor halt
echo === g_rb_probe_idx @0x2000d168 ===\n
x/1xw 0x2000d168
echo === g_rb_uart_addr[0..13] @0x2000d16c ===\n
x/14xw 0x2000d16c
echo === g_rb_uart_probe (vtab) [0..13] @0x2000d1cc ===\n
x/14xw 0x2000d1cc
echo === END ===\n
