set pagination off
target extended-remote localhost:3333
monitor halt
printf "=== PC = 0x%08X ===\n", $pc
printf "cfsr=0x%08X hfsr=0x%08X\n", *(unsigned*)0xE000ED28, *(unsigned*)0xE000ED2C
printf "g_log_queue_up=%d\n", &g_log_queue_up ? *(unsigned*)&g_log_queue_up : -1
printf "g_running(pc of current task)=0x%08X\n", &g_running ? *(unsigned*)&g_running : 0
printf "ms_priority g_app_loaded=%d\n", &g_app_loaded ? *(unsigned*)&g_app_loaded : -1
detach
