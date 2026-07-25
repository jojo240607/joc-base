import serial, threading, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
N = 65536
ser = serial.Serial(port, baudrate=115200, timeout=0.2)
ser.write_timeout = 0
ser.reset_input_buffer()
payload = bytes((i * 73 + 11) & 0xFF for i in range(N))
collected = bytearray()
stop = False

def reader():
    while not stop:
        c = ser.read(1024)
        if c:
            collected.extend(c)

rt = threading.Thread(target=reader, daemon=True)
rt.start()

# Phase 1: blast a big 4096-byte write burst to provoke back-pressure / stall.
t0 = time.time()
sent = 0
while sent < N:
    n = ser.write(payload[sent:sent + 4096])
    sent += n
    if n == 0:
        time.sleep(0.001)
print(f"phase1: sent={sent} collected={len(collected)} in {time.time()-t0:.2f}s")

# Phase 2: STOP writing; only let the reader drain IN (host resumes IN tokens).
# If the device recovered, the remaining echoed bytes should arrive.
t1 = time.time()
while len(collected) < N and (time.time() - t1) < 10.0:
    time.sleep(0.05)
stop = True
dt = time.time() - t1
ok = bytes(collected[:N]) == payload
print(f"phase2: collected={len(collected)}/{N} in {dt:.2f}s recover={'YES' if ok else 'NO'}")
sys.exit(0 if ok else 1)
