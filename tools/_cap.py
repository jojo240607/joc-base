import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
dur = int(sys.argv[2]) if len(sys.argv) > 2 else 12

s = serial.Serial(port, 115200, timeout=0.5)
s.dtr = False
s.rts = False
time.sleep(1)
t = time.time()
n = 0
while time.time() - t < dur:
    raw = s.read(64)
    if raw:
        n += len(raw)
        sys.stdout.write(raw.decode(errors="replace"))
sys.stdout.write("\n[TOTAL %d bytes]\n" % n)
s.close()
