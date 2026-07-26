#!/usr/bin/env python3
"""PC 陪测：板子烧录后在 COM8 上依次跑 RTOSBUS / RTOSUSR / RTOSALL 并验证。

捕获每条命令的 summary 行（RTOSBUS PASS/FAIL 等）以及 selftest 日志，
全部落盘到 flash_out.log 旁便于排查。
"""
import sys, time, serial

PORT = "COM8"
BAUD = 115200

def drain_boot(ser, cap=45):
    """读到串口静默 3 次即认为 boot 完成，进入命令循环。"""
    silent = 0
    t0 = time.time() + cap
    while time.time() < t0:
        line = ser.readline().decode(errors="replace").strip()
        if line:
            silent = 0
        else:
            silent += 1
            if silent >= 3:
                break
    ser.reset_input_buffer()

def run_cmd(ser, cmd, cap=60):
    """发送命令，读到 summary 行（命令名 + PASS/FAIL）后继续收 tail 日志。
    返回 ('PASS'|'FAIL'|None, 完整捕获文本)。
    """
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    buf = []
    summary = None
    pattern = cmd + " "
    t0 = time.time() + cap
    while time.time() < t0:
        line = ser.readline().decode(errors="replace")
        if not line:
            continue
        s = line.strip()
        buf.append(s)
        # 命令会被本地回显，忽略之
        if s == cmd:
            continue
        print("  board> " + s)
        if s.startswith(pattern):
            rest = s[len(pattern):].strip()
            if rest in ("PASS", "FAIL"):
                summary = rest
                # 收尾 0.6s 余下日志（selftest 的零散行）
                tail = time.time() + 0.6
                while time.time() < tail:
                    l2 = ser.readline().decode(errors="replace")
                    if not l2:
                        break
                    s2 = l2.strip()
                    if s2:
                        buf.append(s2)
                        print("  board> " + s2)
                break
    return summary, "\n".join(buf)

def main():
    print("[verify] opening %s @ %d ..." % (PORT, BAUD))
    ser = serial.Serial(PORT, BAUD, timeout=2.0)
    print("[verify] draining boot ...")
    drain_boot(ser)
    print("[verify] boot done; issuing commands\n")

    results = {}
    for cmd in ("RTOSBUS", "RTOSUSR", "RTOSALL"):
        print("==== %s ====" % cmd)
        st, _ = run_cmd(ser, cmd)
        results[cmd] = st
        print("---- %s -> %s\n" % (cmd, st))

    ser.close()
    ok = all(v == "PASS" for v in results.values())
    print("[verify] RESULTS:")
    for k, v in results.items():
        print("  %-8s: %s" % (k, v if v else "NO-RESPONSE"))
    print("[verify] OVERALL: %s" % ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
