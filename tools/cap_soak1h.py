import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
dur  = float(sys.argv[3]) if len(sys.argv) > 3 else 3700.0  # 1h soak + 余量

ser = serial.Serial(port, baud, timeout=0.2)
ser.dtr = False
ser.rts = False
time.sleep(10.0)  # 等 BIST 跑完、控制台就绪
ser.reset_input_buffer()

def send(s):
    ser.write((s + "\n").encode())
    time.sleep(0.05)

# 等到命令循环就绪
send("")  # 触发提示
time.sleep(1.0)
# 启动 1h 长时 soak（acc_b1_soak(ACC_SOAK_LONG_MS=3600000)）
send("RTOSACCEPT long")
print("[soak] RTOSACCEPT long started (target 3600000ms=1h)")

t0 = time.time()
last_ping = t0
buf = b""
while time.time() - t0 < dur:
    d = ser.read(4096)
    if d:
        buf += d
        sys.stdout.buffer.write(d)
        sys.stdout.buffer.flush()
    # 每 5 分钟 PING 一次验证响应（soak 期间控制台仍应响应）
    if time.time() - last_ping > 300.0:
        send("PING")
        last_ping = time.time()
        print("\n[t+%.0fs] PING sent (soak ongoing)" % (time.time() - t0), flush=True)

ser.close()
open("cap_soak_1h.bin", "wb").write(buf)
print("\n[done] bytes=%d saved cap_soak_1h.bin" % len(buf))
