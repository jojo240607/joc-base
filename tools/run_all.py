import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
CMD  = sys.argv[2] if len(sys.argv) > 2 else "RTOSALL"
WAIT = float(sys.argv[3]) if len(sys.argv) > 3 else 45.0

ser = serial.Serial(PORT, 115200, timeout=0.5)
ser.dtr = False; ser.rts = False
time.sleep(0.5)

print("[run] waiting for command prompt (BIST done)...")
t0 = time.time()
while time.time() - t0 < 40:
    ser.reset_input_buffer()
    ser.write(b"PING\n")
    time.sleep(0.6)
    if b"PONG" in ser.read(200):
        break
    time.sleep(0.5)

ser.reset_input_buffer()
ser.write((CMD + "\n").encode())
t0 = time.time()
buf = b""
while time.time() - t0 < WAIT:
    chunk = ser.read(4096)
    if chunk:
        buf += chunk
        sys.stdout.write(chunk.decode("latin1", "replace"))
print("\n----- liveness (PING) -----")
ser.reset_input_buffer()
ser.write(b"PING\n")
time.sleep(1.0)
pong = ser.read(200)
alive = b"PONG" in pong
ser.close()
print("PROMPT_CMD_DONE" if (b"[SELFTEST] ALL" in buf or CMD+" PASS" in buf) else "NO_ALL_MARKER",
      "CONSOLE_ALIVE" if alive else "CONSOLE_DEAD")
