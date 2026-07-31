import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
ser = serial.Serial(port, 115200, timeout=1.0)
ser.dtr = False
ser.rts = False
time.sleep(2.0)
ser.reset_input_buffer()
# send PING
ser.write(b"PING\r\n")
t0 = time.time()
buf = b""
while time.time() - t0 < 10.0:
    d = ser.read(300)
    if d:
        buf += d
print("PORT", port, "REPLY_BYTES", len(buf))
print(repr(buf[:2000]))
ser.close()
