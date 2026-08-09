set pagination off
file build_rel/stm32f407_minimal.elf
target extended-remote localhost:3333
monitor halt
info registers pc
info symbol $pc
x/1xw &g_fault_cfsr
x/1xw &g_fault_pc
bt
detach
quit
