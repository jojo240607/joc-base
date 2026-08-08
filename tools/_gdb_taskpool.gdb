set pagination off
set confirm off
set print pretty off
set remotetimeout 15
# 连接到已运行的 OpenOCD (端口 3333)
target remote 127.0.0.1:3333
# 让板子复位并运行起来，system 任务会调用 rust_app_start 创建 Rust 任务
monitor reset run
# 等待足够长时间让 RTOS 启动 + Rust 任务挂载（OpenOCD 侧 sleep）
monitor sleep 4000
printf "=== g_task_pool scan (CCM 0x10002200) ===\n"
set $n = 48
set $i = 0
while $i < $n
  set $t = &g_task_pool[$i]
  set $st = (int)($t->state)
  if $st != 0
    printf "[%02d] name=%-16s prio=%d state=%d rt_class=%d\n", $i, (char*)($t->name), (int)($t->prio), $st, (int)($t->rt_class)
  end
  set $i = $i + 1
end
printf "=== g_task_count = %d ===\n", (int)g_task_count
printf "=== look for Rust tasks ===\n"
set $j = 0
while $j < $n
  set $t = &g_task_pool[$j]
  set $st = (int)($t->state)
  if $st != 0 && ((char*)($t->name))[0] == 'r' && ((char*)($t->name))[1] == 'u'
    printf "RUST TASK FOUND: [%02d] name=%s prio=%d state=%d\n", $j, (char*)($t->name), (int)($t->prio), (int)($t->state)
  end
  set $j = $j + 1
end
# 停住目标，避免板子继续运行
monitor reset halt
quit
