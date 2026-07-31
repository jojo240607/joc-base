import serial, time, sys, re

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
ALL_RE = re.compile(r"\[SELFTEST\] ALL:")
REPLY_RE = re.compile(r"^(PING|PONG|RTOSALL)\b.*(PASS|FAIL)")
ser = serial.Serial(port, 115200, timeout=1.0)
ser.dtr = False; ser.rts = False
time.sleep(2.0)   # 等板子 UART 起来（开端口 DTR 跳变可能复位板子）
ser.reset_input_buffer()
ser.write(b"RTOSALL\r\n")
lines = []
t0 = time.time()
while time.time() - t0 < 90:
    raw = ser.readline()
    if not raw:
        continue
    line = raw.decode(errors="replace").rstrip("\r\n")
    lines.append(line)
    if ALL_RE.match(line):
        break
    if REPLY_RE.match(line):
        break
ser.close()
open("diag_wdt_full.txt", "w").write("\n".join(lines))
print("TOTAL LINES", len(lines))
for line in lines:
    if "watchdog" in line.lower() or "wdt" in line.lower() or "[SELFTEST]" in line or "FAIL" in line:
        print(line)
