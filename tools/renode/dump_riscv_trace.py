# -*- coding: utf-8 -*-
# RISC-V debug: dump fault record / fault frame regs / switch trace / g_running / TCB pool
# Usage: include after the machine has crashed & paused.
import sys, traceback

OUTF = r"d:\projects\mcu\os\joc-base\tools\renode\riscv_dump.txt"
_f = open(OUTF, "w")

def out(s):
    try:
        sys.stdout.write(s)
        sys.stdout.flush()
    except Exception:
        pass
    try:
        _f.write(s)
        _f.flush()
    except Exception:
        pass

try:
    SB = self.Machine.SystemBus

    def rd32(a):
        try:
            return SB.ReadDoubleWord(a)
        except Exception:
            return 0xDEADBEEF

    def rd8(a):
        try:
            return SB.ReadByte(a)
        except Exception:
            return 0

    def cstr(a, n=24):
        s = []
        for i in range(n):
            c = rd8(a + i)
            if c == 0:
                break
            s.append(chr(c))
        return "".join(s)

    TR_RING   = 0x3fc91638
    TR_CNT    = 0x3fc801d4
    SW_TRACE  = 0x3fc90634
    SW_CNT    = 0x3fc80190
    G_RUNNING = 0x3fc87a4c
    G_TASK_POOL = 0x3fc87a50
    FAULT     = 0x3fc801c4   # mtval, mepc, mcause, count

    out("\n===== fault record =====\n")
    fcnt  = rd32(FAULT + 12)
    fmc   = rd32(FAULT + 8)
    fmepc = rd32(FAULT + 4)
    fmtval = rd32(FAULT + 0)
    out("  count=%d mcause=0x%08X mepc=0x%08X mtval=0x%08X\n" % (fcnt, fmc, fmepc, fmtval))
    if fmc == 4:   # load address misaligned -> mtval = faulting addr = 4 + s1
        out("  -> load-misaligned: faulting addr = mtval = 0x%08X, so s1(old g_running) = 0x%08X\n"
            % (fmtval, (fmtval - 4) & 0xFFFFFFFF))

    out("\n===== fault trap frame (frame_sp from trap ring last entry) =====\n")
    n = rd32(TR_CNT)
    last_idx = (n - 1) & 7
    fsp = rd32(TR_RING + last_idx * 16 + 8)
    out("  frame_sp=0x%08X (trap ring last)\n" % fsp)
    if fsp and 0x3fc00000 <= fsp <= 0x3fcfffff:
        out("    ra (x1)  @+0x00 = 0x%08X\n" % rd32(fsp + 0x00))
        out("    s0 (x8)  @+0x18 = 0x%08X\n" % rd32(fsp + 0x18))
        out("    s1 (x9)  @+0x1C = 0x%08X   <- s1 at fault (= g_running cur)\n" % rd32(fsp + 0x1C))
        out("    s2 (x18) @+0x40 = 0x%08X\n" % rd32(fsp + 0x40))
        out("    s3 (x19) @+0x44 = 0x%08X\n" % rd32(fsp + 0x44))
        out("    s4 (x20) @+0x48 = 0x%08X\n" % rd32(fsp + 0x48))
        out("    a0 (x10) @+0x20 = 0x%08X\n" % rd32(fsp + 0x20))
        out("    mepc     @+0x78 = 0x%08X\n" % rd32(fsp + 0x78))
        out("    mstatus  @+0x7C = 0x%08X\n" % rd32(fsp + 0x7C))

    out("\n===== RISC-V trap ring (%d traps) =====\n" % n)
    for i in range(8):
        idx = (n - 8 + i) & 7 if n >= 8 else i
        mcause = rd32(TR_RING + idx * 16 + 0)
        mepc   = rd32(TR_RING + idx * 16 + 4)
        fsp2   = rd32(TR_RING + idx * 16 + 8)
        out("  trap#%02d: mcause=0x%08X mepc=0x%08X frame_sp=0x%08X\n" % (i, mcause, mepc, fsp2))

    out("\n===== RISC-V switch trace (%d switches) =====\n" % rd32(SW_CNT))
    ns = rd32(SW_CNT)
    for i in range(16):
        idx = (ns - 16 + i) & 15 if ns >= 16 else i
        cur_name = rd32(SW_TRACE + idx * 16 + 0)
        old_sp   = rd32(SW_TRACE + idx * 16 + 4)
        nxt_name = rd32(SW_TRACE + idx * 16 + 8)
        nxt_sp   = rd32(SW_TRACE + idx * 16 + 12)
        out("  sw#%02d: %-8s -> %-8s  old_sp=0x%08X nxt_sp=0x%08X\n" % (
            i, cstr(cur_name), cstr(nxt_name), old_sp, nxt_sp))

    g = rd32(G_RUNNING)
    out("\ng_running=0x%08X\n" % g)
    SZT = 0x64   # sizeof(task_t)=100 (.bss sequential)
    for i in range(12):
        base = G_TASK_POOL + i * SZT
        nm = rd32(base + 4)      # name @ +4
        sp = rd32(base + 0)      # sp @ +0
        st = rd8(base + 11)      # state @ +11
        prio = rd8(base + 8)
        tag = ""
        if g == base:
            tag = "  <<< g_running"
        out("  pool[%02d] @0x%08X prio=%-3d state=%-2d sp=0x%08X name=\"%s\"%s\n" % (
            i, base, prio, st, sp, cstr(nm), tag))

    out("\n===== dump done =====\n")
except Exception:
    _f.write("\n===== PYTHON EXCEPTION =====\n")
    _f.write(traceback.format_exc())
    _f.flush()
finally:
    _f.close()
