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
while time.time() - t0 < 5.0:
    d = ser.read(300)
    if d:
        buf += d
print("PING REPLY_BYTES", len(buf))
print(repr(buf[:2000]))
# send RUST
ser.reset_input_buffer()
ser.write(b"RUST\r\n")
t0 = time.time()
buf2 = b""
while time.time() - t0 < 5.0:
    d = ser.read(300)
    if d:
        buf2 += d
print("RUST REPLY_BYTES", len(buf2))
print(repr(buf2[:2000]))
ser.close()
