import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 10

def wait_prompt(ser, timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        ser.reset_input_buffer()
        ser.write(b"PING\n")
        time.sleep(0.6)
        if b"PONG" in ser.read(200):
            return True
        time.sleep(0.4)
    return False

ser = serial.Serial(PORT, 115200, timeout=0.5)
ser.dtr = False; ser.rts = False
time.sleep(0.5)

pass_n = 0
for r in range(1, ROUNDS + 1):
    ok_prompt = wait_prompt(ser)
    ser.reset_input_buffer()
    ser.write(b"RTOSIRQ\n")
    t0 = time.time()
    buf = b""
    while time.time() - t0 < 16:
        chunk = ser.read(4096)
        if chunk: buf += chunk
    # 收尾存活检查
    ser.reset_input_buffer()
    ser.write(b"PING\n")
    time.sleep(1.0)
    pong = ser.read(200)
    alive = b"PONG" in pong
    passed = (b"RTOSIRQ PASS" in buf) and alive
    pass_n += 1 if passed else 0
    print(f"round {r:2d}: prompt={ok_prompt} RTOSIRQ_PASS={b'RTOSIRQ PASS' in buf} "
          f"alive={alive} -> {'PASS' if passed else 'FAIL'}")
    sys.stdout.flush()

ser.close()
print(f"=== IRQ soak: {pass_n}/{ROUNDS} rounds fully PASS (no crash/hang) ===")
