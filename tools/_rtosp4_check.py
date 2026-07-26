import sys, time, serial
ser = serial.Serial("COM8", 115200, timeout=1)
time.sleep(0.3)
ser.reset_input_buffer()
def send(cmd):
    ser.write((cmd + "\r\n").encode())
    time.sleep(0.05)
for cmd in ("PING", "RTOSP4", "RTOSP4", "RTOSALL", "RTOSALL"):
    send(cmd)
    t0 = time.time()
    while time.time() - t0 < 6:
        try:
            b = ser.read(300)
        except Exception as e:
            print("read err", e); break
        if b:
            sys.stdout.write(b.decode(errors="replace"))
            sys.stdout.flush()
ser.close()
