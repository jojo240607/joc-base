#!/usr/bin/env python3
"""rtosall_check.py — 烧录后跑 RTOSALL 并校验调度器链表完整性。

用法:
    python tools/rtosall_check.py [PORT] [BAUD] [SECS] [OUT]

默认: PORT=COM8 BAUD=115200 SECS=70 OUT=rtosall_cap.bin

行为:
    1. 打开串口（关闭 DTR/RTS，避免 CH340 复位脉冲吞掉 boot 输出）。
    2. 等 boot BIST 完成后发送 "RTOSALL\n"。
    3. 抓取 SECS 秒原始流，落盘 OUT。
    4. 解析文本，打印关键结果行，并重点检测：
         - "[SELFTEST] ALL: PASS/FAIL"（整体结论）
         - P4 里的 "sched_fail="（链表不变量破坏计数，应为 0）
         - 任意 "FAIL" 字样的行（便于快速定位倒退）
    5. 退出码: 0 = ALL PASS 且 sched_fail=0；否则 1。

说明:
    链表完整性断言 RTOS_SCHED_ASSERT 命中时不停机/不异常，只递增
    g_sched_invariant_fail（见 src/rtos/core/rtos_internal.h）。本脚本把它作为
    硬实时内核自检的硬指标：任何非零都意味着存在 TCB 双挂/链表损坏风险。
"""
import sys, serial, time

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
secs = int(sys.argv[3]) if len(sys.argv) > 3 else 70
out  = sys.argv[4] if len(sys.argv) > 4 else "rtosall_cap.bin"

ser = serial.Serial(port, baud, timeout=0.2)
ser.dtr = False   # 避免 CH340 DTR 脉冲复位板子（否则捕获不到 boot 输出）
ser.rts = False
time.sleep(3.0)   # 等 boot BIST 跑完、控制台就绪
ser.write(b"\n")
time.sleep(0.3)
ser.write(b"RTOSALL\n")
buf = b""
t0 = time.time()
while time.time() - t0 < secs:
    try:
        d = ser.read(4096)
    except Exception as e:
        print("READ_ERR", e); break
    if d:
        buf += d
ser.close()
with open(out, "wb") as f:
    f.write(buf)

txt = buf.decode("utf-8", "replace")
lines = txt.splitlines()

# 打印关心的行
want = ("[SELFTEST]", "[P4]", "RESULT", "ALL:", "sched_fail", "TC-KERNEL",
        "TC-TASK", "TC-MTX", "TC-SEM", "TC-EVT", "StackWatermark")
for line in lines:
    if any(w in line for w in want):
        print(line)

# 重点检测
all_pass = any("[SELFTEST] ALL: PASS" in l for l in lines)
all_fail = any("[SELFTEST] ALL: FAIL" in l for l in lines)
# sched_fail：取最后一个出现的值
sched_fail = None
for l in lines:
    if "sched_fail=" in l:
        try:
            sched_fail = int(l.split("sched_fail=")[1].split()[0])
        except Exception:
            pass
# 硬实时违约
hard_rt_viol = any("HARD-RT VIOLATION" in l for l in lines)

print("----")
print("ALL_PASS =", all_pass, " ALL_FAIL =", all_fail,
      " sched_fail =", sched_fail, " hard_rt_violation =", hard_rt_viol)
if sched_fail is not None and sched_fail != 0:
    print("[WARN] g_sched_invariant_fail != 0 -> 链表曾被破坏，见 g_sched_bad_tcb/g_sched_bad_line")
    rc = 1
elif hard_rt_viol:
    print("[FAIL] 硬实时违约：有硬实时任务突破截止期/WCET（见 HARD-RT VIOLATION 行）")
    rc = 1
elif all_fail:
    print("[FAIL] RTOSALL 整体失败")
    rc = 1
elif not all_pass:
    print("[WARN] 未捕获到明确的 ALL: PASS（可能超时/未完成），请检查捕获时长")
    rc = 1
else:
    print("[OK] RTOSALL PASS 且 sched_fail=0 且无硬实时违约：硬实时升级 + 链表防御生效")
    rc = 0

print("TOTAL_BYTES", len(buf))
sys.exit(rc)
