import serial, time

LOG = "cap_verify_p23_com8.log"
out = open(LOG, "w", buffering=1)

def log(*a):
    line = " ".join(str(x) for x in a)
    print(line)
    out.write(line + "\n")
    out.flush()

port = "COM8"
s = serial.Serial(port, 115200, timeout=0.5)
s.dtr = False
s.rts = False
time.sleep(1)
s.reset_input_buffer()
s.write(b"PING\r\n")
t0 = time.time()
alive = False
while time.time() - t0 < 3.0:
    d = s.read(4096)
    if b"PONG" in d:
        alive = True
        break
log("PING->PONG alive=%s" % alive)
if not alive:
    log("BOARD_NOT_ALIVE")
    s.close()
    raise SystemExit(1)

def run_cmd(cmd, wait):
    s.reset_input_buffer()
    s.write((cmd + "\r\n").encode())
    t0 = time.time()
    buf = b""
    while time.time() - t0 < wait:
        d = s.read(4096)
        if d:
            buf += d
    return buf.decode("utf-8", "replace")

acc = run_cmd("RTOSACCEPT", wait=95.0)
log("==== RTOSACCEPT (P2-2 C4 + P2-3 A6) ====")
keys = ("ACC-C4", "IPC-WORST", "ACC-A5", "ACC-A6", "RTOSACCEPT", "RESULT",
        "FAIL", "ALL:", "SELFTEST", "suite:", "LEAK")
for line in acc.splitlines():
    if any(k in line for k in keys):
        log(line)

s.close()
log("VERIFY_P23_COM8_DONE")
