import socket, time, sys

HOST = "localhost"
PORT = 4444

def main():
    f = open("ocd_fault_out.txt", "w", encoding="utf-8")
    def out(s):
        f.write(s + "\n")

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect((HOST, PORT))
    time.sleep(0.3)

    def cmd(c, wait=0.4):
        s.sendall((c + "\n").encode())
        time.sleep(wait)
        data = b""
        s.settimeout(2)
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
                if b"> " in chunk:
                    break
        except socket.timeout:
            pass
        return data.decode("latin-1", errors="replace")

    out(cmd("halt", wait=1.0))

    # ---- fault globals (decoded) ----
    out("=== g_robust_fault_active @0x20004c8c ===")
    out(cmd("mdw 0x20004c8c 1"))
    out("=== g_robust_fault_cfsr @0x20004c88 ===")
    out(cmd("mdw 0x20004c88 1"))
    out("=== g_fault_pc @0x20004c84 ===")
    out(cmd("mdw 0x20004c84 1"))
    out("=== g_fault_pc_raw @0x20004c74 ===")
    out(cmd("mdw 0x20004c74 1"))
    out("=== g_fault_lr (EXC_RETURN) @0x20004c7c ===")
    out(cmd("mdw 0x20004c7c 1"))
    out("=== g_fault_frame @0x20004c78 ===")
    out(cmd("mdw 0x20004c78 1"))
    out("=== g_fault_task_name @0x20004c5c (6 words) ===")
    raw = cmd("mdw 0x20004c5c 6")
    out(raw)
    # decode the task name string
    name = ""
    for line in raw.splitlines():
        idx = line.find(":")
        if idx < 0: continue
        parts = line[idx+1:].split()
        for w in parts:
            try:
                v = int(w, 16)
            except ValueError:
                continue
            for b in range(4):
                c = (v >> (b*8)) & 0xFF
                if c == 0: break
                name += chr(c)
    out("DECODED task name: " + repr(name))

    # ---- rb probe arrays ----
    out("=== g_rb_probe_idx @0x2000d168 ===")
    out(cmd("mdw 0x2000d168 1"))
    out("=== g_rb_uart_addr[16] @0x2000d16c ===")
    out(cmd("mdw 0x2000d16c 16"))
    out("=== g_rb_uart_probe[16] @0x2000d1cc ===")
    out(cmd("mdw 0x2000d1cc 16"))

    # ---- live state ----
    out("=== g_app_ctx @0x20000000 (8) ===")
    out(cmd("mdw 0x20000000 8"))
    out("=== g_console @0x20000948 ===")
    out(cmd("mdw 0x20000948 1"))

    # CFSR bit decode
    try:
        cfsr = int(cmd("mdw 0x20004c88 1").split(":")[1].split()[0], 16)
    except Exception:
        cfsr = -1
    if cfsr >= 0:
        bits = []
        if cfsr & (1<<25): bits.append("DIVBYZERO")
        if cfsr & (1<<24): bits.append("UNALIGNED")
        if cfsr & (1<<16): bits.append("UNDEFINSTR")
        if cfsr & (1<<9):  bits.append("STKOF")
        if cfsr & (1<<8):  bits.append("NOCP")
        if cfsr & (1<<0):  bits.append("IACCVIOL")
        if cfsr & (1<<1):  bits.append("DACCVIOL")
        if cfsr & (1<<3):  bits.append("MUNSTKERR")
        if cfsr & (1<<4):  bits.append("MSTKERR")
        if cfsr & (1<<7):  bits.append("MMARVALID")
        out("CFSR decode: " + " ".join(bits) + (" (0x%08x)" % cfsr))

    f.close()
    print("DONE wrote ocd_fault_out.txt")

if __name__ == "__main__":
    main()
