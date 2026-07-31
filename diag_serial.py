import serial, time, sys
port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
cmd = sys.argv[2] if len(sys.argv) > 2 else "PING"
ser = serial.Serial(port, 115200, timeout=0.5, write_timeout=3.0)
ser.dtr = False
ser.rts = False
time.sleep(1.5)
ser.reset_input_buffer()
n = ser.write((cmd + "\r\n").encode())
print("SENT", cmd, "wrote", n, "bytes")
t0 = time.time()
buf = b""
while time.time() - t0 < 8:
    d = ser.read(300)
    if d:
        buf += d
print("RECV %d bytes on %s:" % (len(buf), port))
print(repr(buf[:2500]))
try:
    print(buf.decode(errors="replace")[:2500])
except Exception:
    pass
ser.close()
