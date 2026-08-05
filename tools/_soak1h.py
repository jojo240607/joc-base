"""后台 1h soak 采集（P1-4 长时实测收尾）。
板子已烧录含 acc_b1_soak_long 的固件；本脚本自动探测 COM 端口、发
"RTOSACCEPT long"，把完整输出落盘 soak1h.log（已由启动命令重定向，这里再写一份
结构化尾部摘要）。CH340 DTR 脉冲会复位板子，必须 dtr=False/rts=False。
"""
import serial, time, sys

LOG = "soak1h_capture.log"
out = open(LOG, "w", buffering=1)

def log(*a):
    line = " ".join(str(x) for x in a)
    print(line, flush=True)
    out.write(line + "\n")

port = "COM8"
s = serial.Serial(port, 115200, timeout=1.0)
s.dtr = False; s.rts = False
time.sleep(1.5)
s.reset_input_buffer()
# 探活：发 PING 等 PONG
s.write(b"PING\n")
time.sleep(2)
d = s.read(200)
if b"PONG" not in d:
    log("CONSOLE_NOT_RESPONDING", repr(d))
    sys.exit(2)
log("FOUND_CONSOLE_PORT", port)
s.reset_input_buffer()
log("SENDING: RTOSACCEPT long")
s.write(b"RTOSACCEPT long\r\n")

# 1h soak + 余量；总等待 3700s
deadline = time.time() + 3700
buf = b""
samples = []
last = time.time()
while time.time() < deadline:
    d = s.read(65536)
    if d:
        buf += d
        # 周期落盘，避免进程被杀丢失
        out.write(d.decode("utf-8", "replace"))
        out.flush()
        now = time.time()
        if now - last > 60:
            last = now
            tail = buf.decode("utf-8", "replace").splitlines()[-3:]
            log("[KEEPALIVE %ds]" % int(now - (deadline - 3700)), *tail)
s.close()

txt = buf.decode("utf-8", "replace")
log("=== SOAK1H DONE (capture %d bytes) ===" % len(buf))
for l in txt.splitlines():
    if "ACC-B1" in l or "B1-DL-SAMPLE" in l or "RESULT" in l or "FAIL" in l:
        log(l)
# 判定
if "ACC_B1_Soak" in txt and "FAIL" not in txt.split("ACC_B1_Soak")[-1]:
    log("SOAK1H_RESULT: PASS")
else:
    log("SOAK1H_RESULT: CHECK_FAIL_OR_INCOMPLETE")
