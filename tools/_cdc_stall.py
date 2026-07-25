import serial, sys, time

cdc_port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
c = serial.Serial(cdc_port, 115200, timeout=0.5, write_timeout=1.0)
u = serial.Serial("COM8", 115200, timeout=2)

payload = bytes((i * 73 + 11) & 0xFF for i in range(3000))
t0 = time.time()
sent = 0
try:
    for i in range(0, len(payload), 64):
        c.write(payload[i:i + 64])
        sent += 64
except Exception as e:
    print("WRITE stopped:", e)
print("CDC wrote ~%d bytes in %.2fs" % (sent, time.time() - t0))

time.sleep(1.0)
u.write(b"USBSTAT\r\n")
time.sleep(1.0)
out = u.read(3000).decode(errors="replace")
for ln in out.splitlines():
    if "EP1" in ln or "line_coding" in ln or "addr=" in ln:
        print(ln)
c.close()
u.close()
