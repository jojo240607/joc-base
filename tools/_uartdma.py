import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
s = serial.Serial(PORT, 115200, timeout=0.5)
s.dtr = False
time.sleep(0.1)
s.dtr = True
time.sleep(2.5)
s.reset_input_buffer()

cmd = sys.argv[2] if len(sys.argv) > 2 else "UARTDMA"
s.write((cmd + "\r\n").encode())
# send RX bytes a few times so at least one burst lands while RX-DMA is armed
for _ in range(5):
    time.sleep(0.15)
    s.write(b"WXYZ")
end = time.time() + 6
buf = b""
while time.time() < end:
    d = s.read(200)
    if d:
        buf += d
        sys.stdout.write(d.decode(errors="replace"))
        sys.stdout.flush()
print("\n---DONE len=%d---" % len(buf))
