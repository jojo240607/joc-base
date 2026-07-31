import serial, time, subprocess, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
OCD = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
SCR = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"

# ---- 1) trigger the repro: clean-ish (we already booted), send RTOSMARATHON twice ----
ser = serial.Serial(PORT, 115200, timeout=0.2)
ser.dtr = False; ser.rts = False
time.sleep(8.0)
ser.reset_input_buffer()
print("[cap] send RTOSMARATHON #1")
ser.write(b"RTOSMARATHON\n")
time.sleep(8.0)
ser.reset_input_buffer()
print("[cap] send RTOSMARATHON #2")
ser.write(b"RTOSMARATHON\n")
ser.close()
print("[cap] waiting 18s for fault to develop...")
time.sleep(18.0)

# ---- 2) dump fault diagnostics via OpenOCD ----
cmds = [
    "init",
    "halt",
    "mdw 0x2000468c 1",   # g_fault_cfsr
    "mdw 0x20004674 1",   # g_fault_frame
    "mdw 0x20004678 1",   # g_fault_lr
    "mdw 0x20004680 1",   # g_fault_pc
    "mdw 0x20004658 6",   # g_fault_task_name
    "mdw 0x20004690 1",   # g_stack_overflow
    "mdw 0x20003668 1",   # g_running (ptr)
    "mdw 0x10003800 256", # TCB pool first ~1KB
    "reg",
    "exit",
]
args = [OCD, "-s", SCR, "-f", "interface/stlink.cfg", "-f", "target/stm32f4x.cfg",
        "-c", "transport select swd"]
for c in cmds:
    args += ["-c", c]
r = subprocess.run(args, capture_output=True, text=True, timeout=60)
print("===== OPENOCD OUTPUT =====")
print(r.stdout)
print(r.stderr)
