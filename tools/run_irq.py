import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 14.0

ser = serial.Serial(PORT, 115200, timeout=0.5)
ser.dtr = False; ser.rts = False
time.sleep(0.5)

# 1) 等待 boot BIST 结束、进入命令提示符（PING 能回 PONG），避免与 BIST 并发污染测量
print("[run] waiting for command prompt (BIST done)...")
t0 = time.time()
at_prompt = False
while time.time() - t0 < 40:
    ser.reset_input_buffer()
    ser.write(b"PING\n")
    time.sleep(0.6)
    resp = ser.read(200)
    if b"PONG" in resp:
        at_prompt = True
        break
    time.sleep(0.5)
print("[run] at prompt:" , at_prompt)
if not at_prompt:
    print("[run] WARNING: never saw PONG; proceeding anyway")

# 2) 发送 RTOSIRQ 并捕获完整报告
ser.reset_input_buffer()
ser.write(b"RTOSIRQ\n")
t0 = time.time()
buf = b""
while time.time() - t0 < WAIT:
    chunk = ser.read(4096)
    if chunk:
        buf += chunk
        sys.stdout.write(chunk.decode("latin1", "replace"))
print("\n----- console liveness check (PING) -----")
ser.reset_input_buffer()
ser.write(b"PING\n")
time.sleep(1.0)
pong = ser.read(200)
print("[ping] response:", pong)
alive = b"PONG" in pong
ser.close()
print("CONSOLE_ALIVE" if alive else "CONSOLE_DEAD")
