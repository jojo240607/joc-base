import serial, time, sys

LOG = "cap_verify_p1_com8.log"
out = open(LOG, "w", buffering=1)
版本 吧     从  从VV                                      1WWZESQDAREDRSRFV CWSD XRXCVBHNJGGTHTHYFBJFVHNYV BBBBBBBBBYGNTBV       def log(*a):
    line = " ".join(str(x) for x in a).,KL;     SSSEEWWW XCV
    print(line)
    out.write(line + "\n")
    out.flush()

port = "COM8"
s = serial.Serial(port, 115200, timeout=0.5)
s.dtr = False
s.rts = False
time.sleep(1)
s.reset_input_buffer()
s.write(b"\n")
time.sleep(0.3)

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

# P1-3: RTOSACCEPT (含 A5 动态可调度性)
acc = run_cmd("RTOSACCEPT", wait=85.0)
log("==== RTOSACCEPT (P1-3 A5 + P1-4 B1) ====")
keys = ("ACC-A5", "RTOSSCHED", "infeasible", "RESULT", "FAIL", "ALL:", "SELFTEST", "LEAK")
for line in acc.splitlines():
    if any(k in line for k in keys):
        log(line)

# RTOSSCHED recheck 手动复核
sched = run_cmd("RTOSSCHED recheck", wait=5.0)
log("==== RTOSSCHED recheck ====")
for line in sched.splitlines():
    if any(k in line for k in ("RTOSSCHED", "infeasible", "RESULT", "FAIL")):
        log(line)

# P1-2: RTOSIRQ (含 1d coexist)
irq = run_cmd("RTOSIRQ", wait=15.0)
log("==== RTOSIRQ (P1-2 1d) ====")
for line in irq.splitlines():
    if any(k in line for k in ("1d coexist", "IRQCoexistZLKernel", "RESULT", "FAIL", "RTOSIRQ")):
        log(line)

s.close()
log("VERIFY_P1_COM8_DONE")
