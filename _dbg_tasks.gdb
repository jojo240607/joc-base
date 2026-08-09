set pagination off
target extended-remote localhost:3333
monitor halt
printf "=== PC = 0x%08X ===\n", $pc
printf "cfsr=0x%08X hfsr=0x%08X\n", *(unsigned*)0xE000ED28, *(unsigned*)0xE000ED2C
# probe symbols
printf "g_log_queue_up=%d\n", *(unsigned*)&g_log_queue_up
printf "g_app_loaded=%d\n", *(unsigned*)&g_app_loaded
# dump first 12 task pool slots: task_t is large; print name + state if accessible
set $p = (char*)&g_task_pool
printf "g_task_pool @ %p\n", $p
# try the kernel query helpers
print rtos_kobj_lookup("log")
print rtos_kobj_lookup("main")
print rtos_kobj_lookup("app_host")
detach
