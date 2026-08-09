set pagination off
target extended-remote localhost:3333
monitor halt
printf "=== PC = 0x%08X ===\n", $pc
printf "cfsr=0x%08X hfsr=0x%08X\n", *(unsigned*)0xE000ED28, *(unsigned*)0xE000ED2C
printf "g_app_loaded=%d g_log_queue_up=%d\n", *(unsigned*)&g_app_loaded, *(unsigned*)&g_log_queue_up
printf "g_task_count=%d\n", &g_task_count ? *(unsigned*)&g_task_count : -1
# peek app_main task stack bottom / name if symbols exist
printf "app_main_task? search via kobj...\n"
# try reading the app_host created flag via g_app_entry
printf "g_app_entry=0x%08X\n", *(unsigned*)&g_app_entry
detach
