import serial, time, sys

PORT = "COM8"
BAUD = 115200
INIT_WAIT = 12          # wait for boot + command prompt
ROBUST_WAIT = 18        # time to let RTOSROBUST run / corrupt

ser = serial.Serial(PORT, BAUD, timeout=0.3)
ser.dtr = False
ser.rts = False
time.sleep(1.0)
print("[sender] waiting %ds for boot..." % INIT_WAIT)
time.sleep(INIT_WAIT)

ser.reset_input_buffer()
ser.write(b"RTOSROBUST\n")
ser.flush()
print("[sender] sent RTOSROBUST")

time.sleep(ROBUST_WAIT)
out = ser.read(8192)
print("=== ROBUST OUT (tail) ===")
print(out.decode(errors="replace")[-900:])

print("=== probing for wedge via PING ===")
for i in range(6):
    ser.reset_input_buffer()
    ser.write(b"PING\n")
    ser.flush()
    time.sleep(2.5)
    r = ser.read(4096).decode(errors="replace")
    if "PONG" in r:
        print("[ping %d] PONG (alive)" % i)
    elif "READY" in r or "Discovery" in r or "Hello" in r:
        print("[ping %d] REBOOT detected" % i)
    else:
        print("[ping %d] WEDGED / no response" % i)
ser.close()
