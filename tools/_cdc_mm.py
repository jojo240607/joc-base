import serial, threading, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
n = int(sys.argv[2]) if len(sys.argv) > 2 else 4096

ser = serial.Serial(port, baudrate=115200, timeout=0.2)
ser.reset_input_buffer()
payload = bytes((i * 73 + 11) & 0xFF for i in range(n))
col = bytearray()
stop = False

def reader():
    while not stop:
        c = ser.read(256)
        if c:
            col.extend(c)

rt = threading.Thread(target=reader, daemon=True)
rt.start()
t0 = time.time()
for i in range(0, n, 64):
    ser.write(payload[i:i + 64])
while len(col) < n and (time.time() - t0) < 10:
    time.sleep(0.01)
stop = True
col = bytes(col[:n])
ok = col == payload
sys.stderr.write("ok=%s len=%d\n" % (ok, len(col)))
sys.stderr.flush()
if not ok:
    for k in range(n):
        if col[k] != payload[k]:
            sys.stderr.write("first mismatch at %d\n" % k)
            sys.stderr.write("exp %s\n" % payload[max(0,k-4):k+12].hex())
            sys.stderr.write("got %s\n" % col[max(0,k-4):k+12].hex())
            sys.stderr.flush()
            break
ser.close()
