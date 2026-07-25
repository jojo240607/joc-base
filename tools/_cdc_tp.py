import serial, sys, threading, time

port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
nbytes = int(sys.argv[2]) if len(sys.argv) > 2 else 65536

ser = serial.Serial(port, baudrate=115200, timeout=0.2)
ser.reset_input_buffer()
ser.write_timeout = 0   # non-blocking: don't hold the port lock while the OS
                        # waits for the (back-pressured) device to accept OUT
print("OPEN", port, "payload", nbytes)

payload = bytes((i * 73 + 11) & 0xFF for i in range(nbytes))

collected = bytearray()
stop = False

def reader():
    while not stop:
        chunk = ser.read(256)
        if chunk:
            collected.extend(chunk)

rt = threading.Thread(target=reader, daemon=True)
rt.start()

t0 = time.time()
# write in small chunks; write_timeout=0 makes each write return immediately with
# the count the OS accepted, so the reader thread can keep draining IN.
# Yield when a write makes no progress: otherwise this tight loop would starve the
# reader thread (pyserial serializes read+write on one lock), and the host would
# stop reading IN -> the device's IN FIFO fills -> back-pressure deadlocks.
sent = 0
while sent < len(payload):
    n = ser.write(payload[sent:sent + 64])
    sent += n
    if n == 0:
        time.sleep(0.001)
while len(collected) < len(payload) and (time.time() - t0) < 20.0:
    time.sleep(0.01)
stop = True
dt = time.time() - t0

ok = bytes(collected[:len(payload)]) == payload
mbps = (len(collected) / 1024 / 1024) / dt if dt > 0 else 0.0
print(f"echoed={len(collected)}/{len(payload)} in {dt*1000:.1f} ms -> {mbps:.2f} MB/s")
print("integrity:", "PASS" if ok else "FAIL")
ser.close()
