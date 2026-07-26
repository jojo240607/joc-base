import serial, time, sys
port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
try:
    ser = serial.Serial(port, 115200, timeout=0.5)
except Exception as e:
    print("open fail:", e); sys.exit(2)
print("opened", port)
t0 = time.time() + 15
while time.time() < t0:
    try:
        line = ser.readline().decode(errors='replace')
    except Exception as e:
        print("read err:", e); break
    if line:
        print(repr(line.rstrip()))
ser.close()
print("done")
