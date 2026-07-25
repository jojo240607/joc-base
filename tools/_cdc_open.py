import serial, sys, time

port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
ser = serial.Serial(port, baudrate=115200, timeout=0.5)
print("OPEN OK")
ser.reset_input_buffer()
probe = b"ECHO loopback-probe-123\r\n"
ser.write(probe)
print("WROTE", len(probe), "bytes")

# Read for up to 4 seconds, accumulating.
got = b""
t0 = time.time()
while time.time() - t0 < 4.0:
    chunk = ser.read(64)
    if chunk:
        got += chunk
        if len(got) >= len(probe):
            break
print("GOT", len(got), "bytes:", repr(got))
print("ECHO", "PASS" if got == probe else "FAIL")
ser.close()
