import sys, time, serial
ser = serial.Serial("COM8", 115200, timeout=0.3)
time.sleep(0.3)
ser.reset_input_buffer()
def send(cmd):
    ser.write((cmd + "\r\n").encode())
    time.sleep(0.2)
# 预先把命令全部发出（板子在 BIST 洪泛期间会缓冲 RX，BIST 结束后按顺序处理）
for cmd in ("PING", "RTOSP4", "RTOSFPU", "RTOSALL"):
    send(cmd)
# 连续读取 200s，保持串口缓冲新鲜（读取远快于 115200 洪泛），全部落盘
t0 = time.time()
while time.time() - t0 < 200:
    b = ser.read(2000)
    if b:
        sys.stdout.write(b.decode(errors="replace"))
        sys.stdout.flush()
print("=== CAPTURE DONE ===")
ser.close()
