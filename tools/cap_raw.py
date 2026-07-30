import sys, serial, time

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
secs = int(sys.argv[3]) if len(sys.argv) > 3 else 55
out  = sys.argv[4] if len(sys.argv) > 4 else "cap_raw.bin"

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
# 打印所有 [P4] / [KOBJ] / RESULT 行（解码容错）
txt = buf.decode("utf-8", "replace")
for line in txt.splitlines():
    if "[P4]" in line or "[KOBJ]" in line or "RESULT" in line or "TC-KERNEL-004" in line or "TC-TASK-005" in line:
        print(line)
print("TOTAL_BYTES", len(buf))
