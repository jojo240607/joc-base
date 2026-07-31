import subprocess, sys

OCD = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
SCR = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
POOL=0x10003800; TCBW=16

def ocd(cmds):
    a=[OCD,"-s",SCR,"-f","interface/stlink.cfg","-f","target/stm32f4x.cfg","-c","transport select swd"]
    for c in cmds: a+=["-c",c]
    r=subprocess.run(a,capture_output=True,text=True,timeout=90)
    return r.stdout+r.stderr

def parse(text,base,n):
    m={}
    for line in text.splitlines():
        line=line.strip()
        if line.startswith("0x") and ":" in line:
            try:
                a=int(line.split(":")[0],16); v=[int(x,16) for x in line.split(":",1)[1].split()]
            except ValueError: continue
            m[a]=v
    addrs=sorted(x for x in m if base<=x<base+n*4)
    w=[]
    for x in addrs: w+=m[x]
    return w[:n]

# scalars (current addresses from nm)
# g_marathon_gen@0x20004c1c (1B), g_marathon_on@0x20004c1d (1B) share a word
cmds=["init","halt",
      "mdw 0x20004c1c 1","mdw 0x20003550 1",
      "mdw 0x20004c0c 4",
      # TCBs 12,13,14,15 full frames + saved stack frames
      f"mdw 0x{POOL+12*64:x} {TCBW}", f"mdw 0x{POOL+13*64:x} {TCBW}",
      f"mdw 0x{POOL+14*64:x} {TCBW}", f"mdw 0x{POOL+15*64:x} {TCBW}",
      "exit"]
text=ocd(cmds)
w=parse(text,0x20004c1c,1)
tc=parse(text,0x20003550,1)
beat=parse(text,0x20004c0c,4)
gen_on=w[0] if w else 0
gen=gen_on & 0xff
on=(gen_on>>8)&0xff
print(f"g_marathon_gen={gen} g_marathon_on={on} g_task_count={tc[0] if tc else '?'}")
print(f"g_marathon_beat[0..3]={[hex(x) for x in beat]}")
for i in (12,13,14,15):
    w=parse(text,POOL+i*64,TCBW)
    sp=w[0]; sb=w[3]; ss=w[4]; entry=w[5]
    pb=w[2]; prio=pb&0xff; state=(pb>>24)&0xff
    STATES={0:"READY",1:"RUN",2:"SLEEP",3:"BLOCK",4:"SUSP",5:"DEAD"}
    print(f"\nTCB idx {i}: prio={prio} state={STATES.get(state,'?')} sp=0x{sp:08x} stack_base=0x{sb:08x} size=0x{ss:08x} entry=0x{entry:08x}")
    # dump saved frame at sp (up to 24 words covering hw+r4-r11+EXC_RETURN, and FPU s16-s31 if present)
    fr=parse(text, sp & ~0x3, 24)
    print(f"  saved frame @0x{sp:08x}:")
    for off in range(0,len(fr)*4,4):
        print(f"    0x{sp+off:08x}: {fr[off//4]:08x}")
