set pagination off
set confirm off
set remotetimeout 60
set logging file gdb_appverify.log
set logging redirect on
set logging on
target extended-remote localhost:3333
monitor reset halt
# 仅断 Rust 关键函数，不在 uart_console_putc 逐字符断（避免 BIST 海量输出拖垮 keepalive）
break rust_app_start
commands
  silent
  printf "*** rust_app_start ENTERED (mount OK) ***\n"
  continue
end
break rust_task_entry
commands
  silent
  printf "*** rust_task_entry SCHEDULED (demo task running) ***\n"
  continue
end
break att_isr_give
commands
  silent
  printf "*** att_isr_give CALLED (TIM6 ISR -> sem_give) ***\n"
  continue
end
continue
