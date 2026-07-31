set pagination off
set print pretty off
file build/stm32f407_minimal.elf
target remote localhost:3333
monitor halt
echo === REGISTERS ===\n
info reg pc lr sp
echo === FAULT GLOBALS ===\n
printf "g_fault_cfsr = 0x%08X\n", g_fault_cfsr
printf "g_fault_pc   = 0x%08X\n", g_fault_pc
printf "g_fault_lr   = 0x%08X\n", g_fault_lr
printf "g_fault_frame= 0x%08X\n", g_fault_frame
printf "g_fault_task = %s\n", g_fault_task_name
echo === UART / APPCTX ===\n
printf "g_app_ctx.uart = 0x%08X\n", (uint32_t)g_app_ctx.uart
printf "uart vtab word = 0x%08X\n", *(uint32_t*)g_app_ctx.uart
echo === BACKTRACE ===\n
bt
