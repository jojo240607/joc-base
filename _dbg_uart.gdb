set pagination off
file build_rel/stm32f407_minimal.elf
target extended-remote localhost:3333
monitor reset run
break uart_console_putc
continue
# 让板子跑 5 秒，期间若有输出会在此断下
echo HIT_COUNT_CHECK\n
info breakpoints
detach
quit
