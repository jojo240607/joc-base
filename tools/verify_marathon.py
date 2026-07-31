import serial, time, subprocess, sys

OCD = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
SCR = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"

# open serial -> CH340 DTR pulse resets board once
ser = serial.Serial(PORT, 115200, timeout=0.5)
ser.dtr = False; ser.rts = False
time.sleep(10.0)                       # BIST
ser.reset_input_buffer()

print("[v] RTOSMARATHON #1")
ser.write(b"RTOSMARATHON\n"); time.sleep(8.0); ser.reset_input_buffer()
print("[v] RTOSMARATHON #2 (restart)")
ser.write(b"RTOSMARATHON\n"); time.sleep(8.0); ser.reset_input_buffer()

# responsiveness check on the SAME session (no reopen -> no extra reset)
print("[v] PING")
ser.write(b"PING\n"); time.sleep(1.5)
resp = ser.read(200)
ser.close()
alive = b"PONG" in resp
print(f"[v] PING response: {resp!r}  -> {'ALIVE' if alive else 'DEAD/blocked'}")

# read CFSR + TCB pool via OpenOCD (halt/read/resume; halt does not reset)
POOL=0x10003800; NT=48; TW=16
cmds=["init","halt", f"mdw 0xE000ED28 1",
      f"mdw 0x{POOL:x} {NT*TW}", "exit"]
a=[OCD,"-s",SCR,"-f","interface/stlink.cfg","-f","target/stm32f4x.cfg","-c","transport select swd"]
for c in cmds: a+=["-c",c]
r=subprocess.run(a,capture_output=True,text=True,timeout=90)
text=r.stdout+r.stderr
m={}
for line in text.splitlines():
    line=line.strip()
    if line.startswith("0x") and ":" in line:
        try:
            ad=int(line.split(":")[0],16); v=[int(x,16) for x in line.split(":",1)[1].split()]
        except ValueError: continue
        m[ad]=v
cfsr=None
for ad,v in m.items():
    if ad==0xE000ED28 and v: cfsr=v[0]
print(f"[v] hardware CFSR = 0x{cfsr:08x}" if cfsr is not None else "[v] CFSR n/a")

# check for duplicate stack_base among marathon TCBs (entry 0x08010b11)
addrs=sorted(x for x in m if POOL<=x<POOL+NT*TW*4)
words=[]
for x in addrs: words+=m[x]
words=words[:NT*TW]
def entry_of(i): return words[i*TW+5]
MAR=0x08010b11
bases=[words[i*TW+3] for i in range(NT) if entry_of(i)==MAR]
print(f"[v] marathon TCB count = {len(bases)}")
dups=set(b for b in bases if bases.count(b)>1)
print(f"[v] duplicate stack bases = {[hex(x) for x in dups] if dups else 'NONE (no aliasing)'}")
