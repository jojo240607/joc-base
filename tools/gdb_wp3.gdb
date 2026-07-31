set pagination off
set print pretty off
file build/stm32f407_minimal.elf
target remote localhost:3333
monitor reset halt
break console_run
commands
  set $uart = (uint32_t)g_app_ctx.uart
  printf "UART base = 0x%08X\n", $uart
  watch *(uint32_t*)$uart
  commands
    printf "*** OBJECT vtable zeroed at 0x%08X ***\n", $uart
    bt
    info reg
    x/16xw $uart
    quit
  end
  watch *(uint32_t*)&g_app_ctx.uart
  commands
    printf "*** g_app_ctx.uart pointer clobbered ***\n"
    bt
    info reg
    x/4xw &g_app_ctx.uart
    quit
  end
  continue
end
continue
