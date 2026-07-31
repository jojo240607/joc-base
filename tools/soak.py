import serial, time, subprocess, sys

OCD = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
SCR = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
MIN = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0   # soak minutes

def cfsr():
    a=[OCD,"-s",SCR,"-f","interface/stlink.cfg","-f","target/stm32f4x.cfg",
       "-c","transport select swd","-c","init","-c","halt",
       "-c","mdw 0xE000ED28 1","-c","resume","-c","exit"]
    r=subprocess.run(a,capture_output=True,text=True,timeout=30)
    for line in (r.stdout+r.stderr).splitlines():
        line=line.strip()
        if line.startswith("0xe000ed28") and ":" in line:
            return int(line.split(":")[1].split()[0],16)
    return None

ser=serial.Serial(PORT,115200,timeout=0.5); ser.dtr=False; ser.rts=False
time.sleep(10.0); ser.reset_input_buffer()
ser.write(b"RTOSMARATHON\n"); time.sleep(3.0)
print(f"[soak] marathon started, soaking {MIN} min")

ops=["RTOSSTRESS\n","RTOSIPC\n","RTOSMPU\n","RTOSFPU\n","RTOSUSR\n","RTOSROBUST\n","PING\n"]
t0=time.time(); i=0; bad=0
while time.time()-t0 < MIN*60:
    op=ops[i % len(ops)]; i+=1
    ser.reset_input_buffer(); ser.write(op.encode()); time.sleep(2.0)
    resp=ser.read(300)
    if b"PONG" not in resp and op.strip()=="PING":
        bad+=1; print(f"[soak] PING MISS @{(time.time()-t0)/60:.1f}min resp={resp!r}")
    else:
        print(f"[soak] {op.strip():10s} ok @{(time.time()-t0)/60:.1f}min")
ser.close()
c=cfsr()
print(f"[soak] final CFSR = {c:#010x} -> {'CLEAN' if (c==0 or c is None) else 'FAULT!'}")
print(f"[soak] PING misses = {bad}")
