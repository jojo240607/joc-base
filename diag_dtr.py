import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
ser = serial.Serial(port, 115200, timeout=1.0, dsrdtr=False, rtscts=False)
# pulse DTR to force a clean reset of the CH340/MCU link
ser.dtr = True
time.sleep(0.1)
ser.dtr = False
ser.rts = False
time.sleep(5.0)   # longer settle
ser.reset_input_buffer()
ser.write(b"PING\r\n")
t0 = time.time()
buf = b""
while time.time() - t0 < 10.0:
    d = ser.read(300)
    if d:
        buf += d
print("PORT", port, "REPLY_BYTES", len(buf))
print(repr(buf[:500]))
ser.close()
