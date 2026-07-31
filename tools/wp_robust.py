import subprocess, time, serial, os

PORT = "COM8"
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=0.5)
ser.dtr = False
ser.rts = False
time.sleep(0.5)
ser.reset_input_buffer()

gdb = r"D:\soft\ST\STM32CubeCLT_1.19.0\GNU-tools-for-STM32\bin\arm-none-eabi-gdb.exe"
outp = "gdb_wp.txt"
if os.path.exists(outp):
    os.remove(outp)

proc = subprocess.Popen([gdb, "-batch", "-x", "tools/gdb_wp.gdb"],
                        stdout=open(outp, "w", encoding="utf-8", errors="replace"),
                        stderr=subprocess.STDOUT)

def send(cmd):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode()); ser.flush()

reached = False
sent_robust = False
for _ in range(150):
    time.sleep(1)
    try:
        with open(outp, encoding="utf-8", errors="replace") as f:
            txt = f.read()
    except Exception:
        txt = ""
    if "REACHED console_run" in txt and not reached:
        reached = True
        print("reached console_run; doing PING handshake", flush=True)
        time.sleep(1)
        send("PING")
    # crude: after PING sent, if we see PONG in serial, send RTOSROBUST
    if reached and not sent_robust:
        # read serial for PONG
        chunk = ser.read(4096)
        if b"PONG" in chunk:
            time.sleep(0.5)
            send("RTOSROBUST")
            sent_robust = True
            print("sent RTOSROBUST after PONG", flush=True)
    if "WP HIT" in txt or "END" in txt:
        print("watchpoint hit / done", flush=True)
        break
try:
    proc.wait(timeout=15)
except subprocess.TimeoutExpired:
    proc.kill()
ser.close()
print("orchestrator done", flush=True)
