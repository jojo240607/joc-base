import socket, time, sys

HOST = "localhost"
PORT = 4444

def main():
    f = open("ocd_dump_out.txt", "w", encoding="utf-8")
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
    out("=== g_app_ctx (0x20000000) ===")
    out(cmd("mdw 0x20000000 4"))
    out("=== UART OBJ @0x20015870 ===")
    out(cmd("mdw 0x20015870 8"))
    out("=== PROBE IDX @0x2000d168 ===")
    out(cmd("mdw 0x2000d168 1"))
    out("=== UART ADDR @0x2000d16c (16) ===")
    out(cmd("mdw 0x2000d16c 16"))
    out("=== UART PROBE @0x2000d1cc (16) ===")
    out(cmd("mdw 0x2000d1cc 16"))

    out("=== SCAN CCM 0x10000000..0x10010000 for 0x08000719/0x08000718 ===")
    found = []
    base = 0x10000000
    span = 0x10000
    step = 1024
    for off in range(0, span, step*4):
        blk = cmd("mdw 0x%x %d" % (base+off, step), wait=0.3)
        for line in blk.splitlines():
            idx = line.find(":")
            if idx < 0: continue
            try:
                addr = int(line[:idx].strip(), 16)
            except ValueError:
                continue
            parts = line[idx+1:].split()
            for i, w in enumerate(parts):
                try:
                    val = int(w, 16)
                except ValueError:
                    continue
                if val in (0x08000719, 0x08000718):
                    found.append(addr + i*4)
    out("FOUND corrupted-LR addresses: " + str([hex(a) for a in found]))
    f.close()
    print("DONE wrote ocd_dump_out.txt")

if __name__ == "__main__":
    main()
