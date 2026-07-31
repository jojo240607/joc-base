#!/usr/bin/env python3
# 长时间多任务压力 soak：
#   1) 启动 RTOSMARATHON（多任务心跳常驻，不带看门狗 -> 安全，不 ARM IWDG）
#   2) 在 hours 小时内循环打压各类 RTOS 自测命令（STRESS/IPC/ROBUST/BH/USR/MPU/FPU）
#   3) 每轮用 PING 探测存活；若 PING 无 PONG 判定挂死/崩溃
#   4) 时间戳日志写入 marathon_stress.log，并打印进度
# 用法: python tools/marathon_stress.py COM8 115200 8
import serial, time, sys, datetime

port   = sys.argv[1] if len(sys.argv) > 1 else "COM8"
baud   = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
hours  = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0
pause  = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0

# 循环打压的“各种运行”命令（不含 RTOSP4/RTOSALL，避免与马拉松常驻任务挤爆 48 槽池）
STRESS_CMDS = ["RTOSSTRESS", "RTOSIPC", "RTOSROBUST",
               "RTOSBH", "RTOSUSR", "RTOSMPU", "RTOSFPU"]

CMD_BUDGET = 20.0   # 单条命令最大读取时长
CMD_QUIET  = 1.5    # 静默多久算该命令输出结束
PING_BUDGET = 6.0
PING_QUIET  = 0.8

log = open("marathon_stress.log", "w", buffering=1)

def ts():
    return datetime.datetime.now().strftime("%H:%M:%S")

def send(ser, s):
    ser.write((s + "\n").encode())

def drain(ser, budget, quiet):
    buf = bytearray()
    t0 = time.time(); last = time.time()
    while True:
        d = ser.read(4096)
        if d:
            buf += d; last = time.time()
        if time.time() - last > quiet:
            break
        if time.time() - t0 > budget:
            break
    return bytes(buf)

def logf(s):
    line = "[%s] %s" % (ts(), s)
    print(line); log.write(line + "\n")

ser = serial.Serial(port, baud, timeout=0.2)
ser.dtr = False   # 规避 CH340 DTR 脉冲复位板子
ser.rts = False
time.sleep(8.0)   # 等 BIST 跑完、控制台就绪
ser.reset_input_buffer()

send(ser, "")
time.sleep(1.0)
send(ser, "RTOSMARATHON")
m = ser.read(4096)
logf("marathon start: %s" % m.decode("utf-8", "replace").strip())

start = time.time()
cycles = 0
fails = 0
crash = False

while time.time() - start < hours * 3600.0:
    cycles += 1
    logf("==== cycle %d ====" % cycles)
    # 打压各类命令
    for cmd in STRESS_CMDS:
        send(ser, cmd)
        out = drain(ser, CMD_BUDGET, CMD_QUIET)
        txt = out.decode("utf-8", "replace")
        for line in txt.splitlines():
            if "FAIL" in line:
                fails += 1
                logf("  FAIL @%s: %s" % (cmd, line.strip()))
        # 每命令后用 PING 探测存活（不解析 fault 文本，避免 RTOSMPU 故意触发 fault 误报）
        send(ser, "PING")
        pout = drain(ser, PING_BUDGET, PING_QUIET)
        if "PONG" in pout.decode("utf-8", "replace"):
            logf("  %s OK (PING->PONG)" % cmd)
        else:
            logf("  %s -> NO PONG (HANG/CRASH)" % cmd)
            crash = True
            break
    if crash:
        break
    time.sleep(pause)

elapsed = time.time() - start
logf("==== SUMMARY: cycles=%d elapsed=%.0fs fails=%d crash=%s ====" %
     (cycles, elapsed, fails, "YES" if crash else "NO"))
logf("STRESS RESULT: %s" % ("ANOMALY DETECTED" if (crash or fails) else "SURVIVED"))
ser.close()
log.close()
