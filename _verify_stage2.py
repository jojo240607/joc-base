import subprocess, time, os, struct

BASE = os.path.dirname(os.path.abspath(__file__))
elf = os.path.join(BASE, "build_stage2", "stm32f407_minimal.elf").replace("\\", "/")
app_bin = os.path.join(BASE, "app.bin").replace("\\", "/")

# 从 app.bin 头部解析 App 入口绝对地址
b = open(app_bin, "rb").read()
magic, ver, entry, size = struct.unpack("<IIII", b[:16])
print(f"app.bin header: magic={magic:#010x} abi_ver={ver} entry={entry:#010x}")
assert magic == 0x41504800, "magic mismatch"
assert 0x08060000 <= entry < 0x08060000 + 0x60000, "entry out of APP_FLASH"

gdb_cmds = f"""
set pagination off
set confirm off
set remotetimeout 60
target extended-remote localhost:3333
monitor reset halt
file {elf}
break app_slot_load_app
commands
  silent
  printf "*** app_slot_load_app CALLED ***\\n"
  printf "    hdr->magic=%p abi=%d entry=%p\\n", hdr->magic, hdr->abi_version, hdr->entry
  continue
end
break *{entry:#010x}
commands
  silent
  printf "*** APP ENTRY REACHED *** sp=0x%08X\\n", $sp
  continue
end
break *0x08001953
commands
  silent
  printf "*** APP CALL RETURNED -> app_main_task resumed at console_run (App ran OK) ***\\n"
  continue
end
break rtos_fault_handler
commands
  silent
  printf "*** FAULT *** cfsr=0x%08X fault_pc=0x%08X mmfar=0x%08X control=0x%08X\\n", *(unsigned*)0xE000ED28, ((unsigned*)frame)[6], *(unsigned*)0xE000ED34, g_fault_control
  printf "  frame r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X\\n", ((unsigned*)frame)[0], ((unsigned*)frame)[1], ((unsigned*)frame)[2], ((unsigned*)frame)[3]
  printf "  frame r12=0x%08X lr=0x%08X pc=0x%08X xpsr=0x%08X\\n", ((unsigned*)frame)[4], ((unsigned*)frame)[5], ((unsigned*)frame)[6], ((unsigned*)frame)[7]
  printf "  msp=0x%08X psp=0x%08X\\n", $msp, $psp
  continue
end
continue
"""
script = os.path.join(BASE, "_verify_stage2.gdb")
open(script, "w").write(gdb_cmds)

log = os.path.join(BASE, "verify_stage2.log")
print("== 验证阶段 2：轨 B 系统从 APP_HEADER 发现并挂载 App ==")
p = subprocess.Popen(
    ["arm-none-eabi-gdb", "-batch", "-x", script, elf],
    stdout=open(log, "w"), stderr=subprocess.STDOUT,
)
print("gdb pid", p.pid, "running ~25s...")
time.sleep(25)
p.terminate()
try: p.wait(timeout=5)
except Exception: p.kill()
print("done ->", log)

txt = open(log, encoding="utf-8", errors="ignore").read()
ok1 = "app_slot_load_app CALLED" in txt
ok2 = "APP ENTRY" in txt and "REACHED" in txt
print(f"[verify] app_slot_load_app called = {ok1}")
print(f"[verify] app entry reached      = {ok2}")
print(f"[verify] app returned to console_run = {'app_main_task' in txt and 'console_run' in txt}")
print("RESULT:", "STAGE-2 PASS" if (ok1 and ok2) else "NEED CHECK")
