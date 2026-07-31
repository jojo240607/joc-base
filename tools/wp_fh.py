import subprocess, time, serial, os

PORT = "COM8"
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=0.5)
ser.dtr = False
ser.rts = False
time.sleep(0.5)
ser.reset_input_buffer()

gdb = r"D:\soft\ST\STM32CubeCLT_1.19.0\GNU-tools-for-STM32\bin\arm-none-eabi-gdb.exe"
outp = "gdb_fh.txt"
if os.path.exists(outp):
    os.remove(outp)

proc = subprocess.Popen([gdb, "-batch", "-x", "tools/gdb_fh.gdb"],
                        stdout=open(outp, "w", encoding="utf-8", errors="replace"),
                        stderr=subprocess.STDOUT)

def send(cmd):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode()); ser.flush()

reached = False
sent = False
for _ in range(120):
    time.sleep(1)
    try:
        with open(outp, encoding="utf-8", errors="replace") as f:
            txt = f.read()
    except Exception:
        txt = ""
    if "REACHED" in txt or "console_run" in txt:
        pass
    if "FAULT HANDLER HIT" in txt:
        print("fault handler hit; dumping", flush=True)
        break
    if not reached:
        # after a few seconds, send PING then RTOSROBUST
        # crude: send PING once, look for PONG, then RTOSROBUST
        if "Breakpoint 1" in txt and not reached:
            reached = True
            time.sleep(1)
            send("PING")
    if reached and not sent:
        chunk = ser.read(4096)
        if b"PONG" in chunk:
            time.sleep(0.5)
            send("RTOSROBUST")
            sent = True
            print("sent RTOSROBUST", flush=True)
try:
    proc.wait(timeout=15)
except subprocess.TimeoutExpired:
    proc.kill()
ser.close()
print("orchestrator done", flush=True)
