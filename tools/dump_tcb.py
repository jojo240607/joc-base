import subprocess, sys

OCD = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
SCR = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
POOL = 0x10003800
NTASKS = 48
TCB_WORDS = 16  # 64 bytes

def ocd(cmds):
    args = [OCD, "-s", SCR, "-f", "interface/stlink.cfg", "-f", "target/stm32f4x.cfg",
            "-c", "transport select swd"]
    for c in cmds:
        args += ["-c", c]
    r = subprocess.run(args, capture_output=True, text=True, timeout=90)
    return r.stdout + r.stderr

def parse(text, base, nwords):
    m = {}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("0x") and ":" in line:
            try:
                a = int(line.split(":")[0], 16)
                vals = [int(x, 16) for x in line.split(":", 1)[1].split()]
            except ValueError:
                continue
            m[a] = vals
    addrs = sorted(a for a in m if base <= a < base + nwords*4)
    words = []
    for a in addrs:
        words += m[a]
    return words[:nwords]

cmds = ["init", "halt",
        f"mdw 0x{POOL:x} {NTASKS*TCB_WORDS}",
        "mdw 0x20003668 1", "mdw 0x2000468c 1", "mdw 0x20004674 1",
        "mdw 0x20004658 6", "exit"]
text = ocd(cmds)
words = parse(text, POOL, NTASKS*TCB_WORDS)
gr = parse(text, 0x20003668, 1)
cf = parse(text, 0x2000468c, 1)
ff = parse(text, 0x20004674, 1)
fn = parse(text, 0x20004658, 6)
g_running = gr[0] if gr else 0
print(f"g_running=0x{g_running:08x}  (idx {(g_running-POOL)//TCB_WORDS if POOL<=g_running<POOL+NTASKS*64 else -1})")
print(f"g_fault_cfsr=0x{cf[0]:08x}  g_fault_frame=0x{ff[0]:08x}")
# task name string: deref each unique name ptr
name_ptrs = set()
for i in range(NTASKS):
    name_ptrs.add(words[i*TCB_WORDS+1])
name_ptrs.discard(0)
ntext = ocd(["init","halt"] + [f"mdw {p} 4" for p in name_ptrs] + ["exit"])
names = {}
nm = parse(ntext, min(name_ptrs), 4*len(name_ptrs)) if name_ptrs else {}
# parse name strings: each mdw 4 gives up to 4 bytes = up to 4 chars
for p in name_ptrs:
    # find words near p
    sub = parse(ntext, p, 4)
    s = b""
    for w in sub:
        s += w.to_bytes(4, "little")
        if b"\x00" in s:
            break
    try:
        names[p] = s.split(b"\x00")[0].decode("ascii", "replace")
    except:
        names[p] = "?"

STATES = {0:"READY",1:"RUN",2:"SLEEP",3:"BLOCK",4:"SUSP",5:"DEAD"}
print("\nidx  sp        name      prio base priv state stack_base stack_size entry     sb_in_CCM?")
for i in range(NTASKS):
    b = i*TCB_WORDS
    sp   = words[b+0]
    name = words[b+1]
    pb   = words[b+2]
    sb   = words[b+3]
    ss   = words[b+4]
    entry= words[b+5]
    prio = pb & 0xff
    base_p = (pb>>8)&0xff
    priv = (pb>>16)&0xff
    state = (pb>>24)&0xff
    nmstr = names.get(name, "?") if name else "<null>"
    ccm = "CCM" if (0x10000000 <= sb < 0x10010000) else ("MSRAM" if (0x20000000<=sb<0x20020000) else "?")
    active = " <== g_running" if (POOL+i*TCB_WORDS)==g_running else ""
    print(f"{i:3d}  {sp:08x}  {name:08x} {prio:3d}  {base_p:3d}  {priv:3d}  {STATES.get(state,'?'):4s}  {sb:08x}  {ss:08x}   {entry:08x}  {ccm}{active}")
