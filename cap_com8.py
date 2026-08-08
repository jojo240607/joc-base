import serial, time, sys
port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0
ser = serial.Serial(port, 115200, timeout=1.0, dsrdtr=False, rtscts=False)
ser.dtr = False
ser.rts = False
time.sleep(0.2)
ser.reset_input_buffer()
# give the board time to boot & run BIST, then send PING
time.sleep(wait)
ser.write(b"PING\r\n")
buf = b""
t0 = time.time()
while time.time() - t0 < 12.0:
    d = ser.read(400)
    if d:
        buf += d
        if b"PONG" in buf:
            break
print("PORT", port, "REPLY_BYTES", len(buf))
print(repr(buf[:3000]))
ser.close()
