import serial, time, subprocess, sys

OCD = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
SCR = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
PORT = sys.argv[1] if len(sys.argv)>1 else "COM8"

# clean reset via OpenOCD
subprocess.run([OCD,"-s",SCR,"-f","interface/stlink.cfg","-f","target/stm32f4x.cfg",
                "-c","reset_config srst_only connect_assert_srst","-c","init",
                "-c","reset halt","-c","resume","-c","exit"],
               capture_output=True,text=True,timeout=30)
time.sleep(2.0)

ser=serial.Serial(PORT,115200,timeout=2.0); ser.dtr=False; ser.rts=False
time.sleep(12.0)                  # generous BIST
def cmd(c, wait):
    ser.reset_input_buffer(); ser.write(c.encode()); time.sleep(wait)
    r=ser.read(2000); print(f"[{c.strip():12s}] -> {r!r}"); return r
cmd("RTOSMARATHON\n", 5.0)
cmd("RTOSSTRESS\n", 15.0)
r=cmd("PING\n", 5.0)
print("PING OK" if b"PONG" in r else "PING FAIL")
cmd("RTOSALL\n", 25.0)
r=cmd("PING\n", 5.0)
print("PING OK" if b"PONG" in r else "PING FAIL")
ser.close()
a=[OCD,"-s",SCR,"-f","interface/stlink.cfg","-f","target/stm32f4x.cfg","-c","transport select swd",
   "-c","init","-c","halt","-c","mdw 0xE000ED28 1","-c","resume","-c","exit"]
out=subprocess.run(a,capture_output=True,text=True,timeout=30).stdout+subprocess.run(a,capture_output=True,text=True,timeout=30).stderr
for line in out.splitlines():
    if "e000ed28" in line and ":" in line:
        print("CFSR =", line.split(":")[1].split()[0])
