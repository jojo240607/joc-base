import serial, time, subprocess, sys

OCD = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
SCR = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"

def ocd(cmds):
    args = [OCD, "-s", SCR, "-f", "interface/stlink.cfg", "-f", "target/stm32f4x.cfg",
            "-c", "transport select swd"]
    for c in cmds:
        args += ["-c", c]
    r = subprocess.run(args, capture_output=True, text=True, timeout=90)
    return r.stdout + r.stderr

# 1) reset board clean
print("[repro] reset board")
ocd(["init", "reset halt", "resume", "exit"])
time.sleep(1.0)

# 2) trigger double RTOSMARATHON
ser = serial.Serial(PORT, 115200, timeout=0.2)
ser.dtr = False; ser.rts = False
time.sleep(8.0)
ser.reset_input_buffer()
print("[repro] RTOSMARATHON #1")
ser.write(b"RTOSMARATHON\n")
time.sleep(8.0)
ser.reset_input_buffer()
print("[repro] RTOSMARATHON #2")
ser.write(b"RTOSMARATHON\n")
ser.close()
print("[repro] wait 35s for fault")
time.sleep(35.0)

# 3) dump TCB pool + fault globals
POOL=0x10003800; NTASKS=48; W=7; nwords=NTASKS*W
cmds=["init","halt", f"mdw 0x{POOL:x} {nwords}",
      "mdw 0x20003668 1","mdw 0x2000468c 1","mdw 0x20004674 1",
      "mdw 0x20004658 6","exit"]
text=ocd(cmds)
m={}
for line in text.splitlines():
    line=line.strip()
    if line.startswith("0x") and ":" in line:
        a=int(line.split(":")[0],16)
        try: vals=[int(x,16) for x in line.split(":",1)[1].split()]
        except ValueError: continue
        m[a]=vals
addrs=sorted(a for a in m if POOL<=a<POOL+nwords*4)
words=[]
for a in addrs: words+=m[a]
words=words[:nwords]
print(f"g_running=0x{m.get(0x20003668,[0])[0]:08x} cfsr=0x{m.get(0x2000468c,[0])[0]:08x} fault_frame=0x{m.get(0x20004674,[0])[0]:08x}")
print("\nidx  sp        name      pb       stack_base stack_size entry")
for i in range(NTASKS):
    b=i*W
    sp,name,pb,sb,ss,entry,arg=words[b:b+7]
    flag=""
    if 0x10000000<=sb<0x10010000: flag+=" <CCM-stack>"
    if sb==0 or sp==0: flag+=" <ZERO>"
    print(f"{i:3d}  {sp:08x}  {name:08x}  {pb:08x}  {sb:08x}    {ss:08x}   {entry:08x}{flag}")
owners=[i for i in range(NTASKS) if words[i*W+4]==0x1000d000]
print(f"\nowners of 0x1000d000: {owners}")
