import subprocess, time, os, signal

# 启动 gdb（后台），跑 gdb_appverify.gdb
proc = subprocess.Popen(
    ["arm-none-eabi-gdb", "-x", "gdb_appverify.gdb", "build/stm32f407_minimal.elf"],
    stdout=open("gdb_run.txt", "w"),
    stderr=subprocess.STDOUT,
)
print("gdb pid", proc.pid, "started")
# 跑 25 秒让板子过 BIST 并调度 Rust 任务
time.sleep(25)
# 终止 gdb（会触发日志 flush）
proc.terminate()
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()
print("gdb stopped")
