set pagination off
file build_rel/stm32f407_minimal.elf
target extended-remote localhost:3333
monitor halt
print/x &g_main_stack
print/x &g_app_loaded
print/x &g_app_entry
print/x &g_app_slot
print/x APP_RAM_BASE
print/x APP_RAM_SIZE
detach
quit
