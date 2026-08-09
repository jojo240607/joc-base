set pagination off
file build_rel/stm32f407_minimal.elf
target extended-remote localhost:3333
monitor halt
echo === current PC ===\n
info registers pc sp
echo === ICSR VECTACTIVE ===\n
x/1xw 0xe000ed04
echo === app_host TCB / stack ===\n
print/x &app_host_stack
print/x &app_host_task_entry
echo === g_app_loaded value ===\n
x/1xw &g_app_loaded
echo === g_task_count ===\n
print g_task_count
echo === bt main ===\n
bt
echo === bt app_host (find tcb) ===\n
detach
quit
