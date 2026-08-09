set pagination off
file build_rel/stm32f407_minimal.elf
target extended-remote localhost:3333
monitor halt
p/x &g_console
p/x g_console
p/x ((console_t*)g_console)->vtable
p/x g_app_ctx.console
p ((console_t*)g_console)->ops.open
info symbol ((console_t*)g_console)->vtable
detach
quit
