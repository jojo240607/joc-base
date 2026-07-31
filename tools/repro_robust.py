import serial, time, re

PORT = "COM8"
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=0.3)
ser.dtr = False
ser.rts = False
time.sleep(1.0)

def drain(w=1.0):
    time.sleep(w)
    return ser.read(8192)

# wait for a fresh boot banner
time.sleep(8)
b = drain(1.0)
print("BOOT BANNER SNIPPET:", b[-200:].decode(errors="replace").replace("\r",""))

def send(cmd, w=6.0):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode()); ser.flush()
    return drain(w)

out = send("RTOSROBUST", 12.0)
print("=== RTOSROBUST tail ===")
print(out.decode(errors="replace")[-600:])

# Now probe for reboot: send PING repeatedly; if a reboot happens we'll see a fresh boot banner
print("=== probing for reboot via repeated PING ===")
for i in range(10):
    r = send("PING", 3.0)
    txt = r.decode(errors="replace")
    if "READY" in txt or "Hello from" in txt or "Discovery" in txt:
        print(f"[ping {i}] REBOOT DETECTED (boot banner present)")
    elif "PONG" in txt:
        print(f"[ping {i}] PONG (alive, no reboot)")
    else:
        print(f"[ping {i}] NO RESPONSE (wedged)")
ser.close()
