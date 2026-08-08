import subprocess, sys, os, time

BASE = os.path.dirname(os.path.abspath(__file__))
sys_bin = os.path.join(BASE, "build_stage2", "stm32f407_minimal.bin")
app_bin = os.path.join(BASE, "app.bin")

if not os.path.exists(sys_bin):
    print(f"[ERR] 缺系统镜像 {sys_bin}，先构建轨 B"); sys.exit(1)
if not os.path.exists(app_bin):
    print(f"[ERR] 缺 {app_bin}，先到 joc-app-rust 跑 python build_app.py 并拷过来"); sys.exit(1)

sys_bin_g = sys_bin.replace("\\", "/")
app_bin_g = app_bin.replace("\\", "/")
gdb_cmds = f"""
target extended-remote localhost:3333
monitor reset halt
monitor flash write_image erase {sys_bin_g} 0x08000000
monitor flash write_image erase {app_bin_g} 0x08060000
monitor reset halt
detach
quit
"""
script = os.path.join(BASE, "_flash_stage2.gdb")
open(script, "w").write(gdb_cmds)

print("== 烧录系统区(0x08000000) + 应用分区(0x08060000) ==")
p = subprocess.run(["arm-none-eabi-gdb", "-batch", "-x", script],
                   cwd=BASE, capture_output=True, text=True)
print(p.stdout)
print(p.stderr)
if "verified" in p.stdout.lower() or "written" in p.stdout.lower():
    print("[OK] 烧录完成")
else:
    print("[WARN] 未检测到 verified，检查上面输出")
