target remote localhost:3333
set height 0
set logging file gdb_log.txt
set logging on
monitor reset halt
break uart_console_putc
commands
  silent
  printf "PUTC:%c\n", (char)$r0
  continue
end
continue
