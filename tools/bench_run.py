import sys, serial, time

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
out  = sys.argv[3] if len(sys.argv) > 3 else "bench_out.txt"

ser = serial.Serial(port, baud, timeout=0.3)
ser.dtr = False   # 防 CH340 DTR 脉冲复位板子
ser.rts = False
time.sleep(3.0)   # 等 boot BIST 完成、控制台就绪
# 先敲个回车，确认控制台在命令循环
ser.write(b"\n")
time.sleep(0.3)
ser.write(b"RTOSBENCH\r\n")

buf = b""
t0 = time.time()
while time.time() - t0 < 6.0:
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
for line in txt.splitlines():
    if "[BENCH]" in line or "[LAT]" in line or "[RTOSBENCH]" in line:
        print(line)
print("TOTAL_BYTES", len(buf))
print("SAVED", out)
