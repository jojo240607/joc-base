import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
dur  = float(sys.argv[3]) if len(sys.argv) > 3 else 40.0

ser = serial.Serial(port, baud, timeout=0.2)
ser.dtr = False
ser.rts = False
time.sleep(8.0)   # 等 BIST 跑完、控制台就绪
ser.reset_input_buffer()

def send(s):
    ser.write((s + "\n").encode())

def ping_at(t):
    time.sleep(t)
    send("PING")
    print("[t+%.0fs] PING sent" % (t,))

# 等到命令循环就绪
send("")  # 触发提示
time.sleep(1.0)
# 启动马拉松（不带 wdt，安全）
send("RTOSMARATHON")
print("[marathon] started (no wdt)")

t0 = time.time()
# 期间多次 PING 验证响应
ping_at(5)
ping_at(20)
ping_at(35)

# 收集输出
buf = b""
while time.time() - t0 < dur:
    d = ser.read(4096)
    if d:
        buf += d
        sys.stdout.write(d.decode("utf-8", "replace"))
        sys.stdout.flush()

ser.close()
open("cap_marathon.bin", "wb").write(buf)
print("\n[done] bytes=%d saved cap_marathon.bin" % len(buf))
