import serial, sys, time

cdc_port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
c = serial.Serial(cdc_port, 115200, timeout=0.5)
u = serial.Serial("COM8", 115200, timeout=0.5)
c.reset_input_buffer()
u.reset_input_buffer()

probe = b"USB-RX-TEST-42\r\n"
c.write(probe)
print("CDC wrote:", probe)

# 1) Did the board RECEIVE it? (mirrored to UART console)
got_u = b""
t0 = time.time()
while time.time() - t0 < 2.5:
    ch = u.read(64)
    if ch:
        got_u += ch
print("UART mirror (RX proof):", repr(got_u))

# 2) Did the board ECHO it back on CDC? (TX proof)
got_c = b""
t0 = time.time()
while time.time() - t0 < 3.0:
    ch = c.read(64)
    if ch:
        got_c += ch
print("CDC echo (TX proof):", repr(got_c))
print("RESULT rx=%s tx=%s" % ("PASS" if probe in got_u else "FAIL",
                               "PASS" if got_c == probe else "FAIL"))
c.close()
u.close()
