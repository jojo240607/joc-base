import serial, time, sys, io
import serial.tools.list_ports as lp

LOG = "cap_verify_p1.log"
out = open(LOG, "w", buffering=1)

def log(*a):
    line = " ".join(str(x) for x in a)
    print(line)
    out.write(line + "\n")
    out.flush()

def find_console():
    ports = [p.device for p in lp.comports()]
    log("PROBE_PORTS", ports)
    for p in ports:
        if p in ("COM1", "COM8"):
            continue
        try:
            s = serial.Serial(p, 115200, timeout=1.0)
            s.dtr = True
            s.rts = True
            time.sleep(8)
            s.reset_input_buffer()
            s.write(b"PING\n")
            time.sleep(3)
            d = s.read(200)
            s.close()
            log("  ", p, "DTR=1 ->", repr(d[:80]))
            if b"PONG" in d:
                return p
            # 再试 dtr=False
            s = serial.Serial(p, 115200, timeout=1.0)
            s.dtr = False
            s.rts = False
            time.sleep(3)
            s.reset_input_buffer()
            s.write(b"PING\n")
            time.sleep(3)
            d = s.read(200)
            s.close()
            log("  ", p, "DTR=0 ->", repr(d[:80]))
            if b"PONG" in d:
                return p
        except Exception as e:
            log("  ", p, "ERR", e)
    return None

def run_cmd(ser, cmd, wait):
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    t0 = time.time()
    buf = b""
    while time.time() - t0 < wait:
        d = ser.read(4096)
        if d:
            buf += d
    return buf.decode("utf-8", "replace")

port = find_console()
if not port:
    log("NO_CONSOLE_FOUND")
    sys.exit(1)
log("CONSOLE_ON", port)

s = serial.Serial(port, 115200, timeout=0.5)
s.dtr = False
s.rts = False
time.sleep(2)
s.write(b"\n")
time.sleep(0.3)

acc = run_cmd(s, "RTOSACCEPT", wait=80.0)
log("==== RTOSACCEPT (P1-3 A5) ====")
for line in acc.splitlines():
    if any(k in line for k in ("ACC-A5", "RTOSSCHED", "infeasible", "RESULT", "FAIL", "ALL:", "SELFTEST")):
        log(line)

sched = run_cmd(s, "RTOSSCHED recheck", wait=5.0)
log("==== RTOSSCHED recheck ====")
for line in sched.splitlines():
    if any(k in line for k in ("RTOSSCHED", "infeasible", "RESULT", "FAIL")):
        log(line)

irq = run_cmd(s, "RTOSIRQ", wait=15.0)
log("==== RTOSIRQ (P1-2 1d) ====")
for line in irq.splitlines():
    if any(k in line for k in ("1d coexist", "IRQCoexistZLKernel", "RESULT", "FAIL")):
        log(line)

s.close()
log("VERIFY_P1_AUTO_DONE")
