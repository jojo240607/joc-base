#!/usr/bin/env python3
"""发送一个 RTOS 自测命令并捕获结果，最后 PING 确认控制台存活。
用法: python tools/run_selftest.py <COMx> <CMD> [timeout_s]
例:   python tools/run_selftest.py COM8 RTOSINV
"""
import sys, time, serial

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    cmd  = sys.argv[2] if len(sys.argv) > 2 else "RTOSALL"
    to   = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0
    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.dtr = False; ser.rts = False
    ser.reset_input_buffer()
    # 等待启动 BIST 结束、出现命令提示符（'>' 行）
    t0 = time.time()
    buf = b""
    while time.time() - t0 < 15:
        c = ser.read(200)
        if c:
            buf += c
            if b"> " in buf[-200:] or b"\r\n> " in buf[-200:]:
                # 再确认 BIST 段已结束
                if b"SELF-TEST" in buf or b"[BIST]" in buf or b"command" in buf:
                    break
    time.sleep(0.3)
    ser.write((cmd + "\r").encode())
    # 捕获自测输出
    t1 = time.time()
    out = b""
    done = False
    while time.time() - t1 < to:
        c = ser.read(300)
        if c:
            out += c
            if (b"self-test: PASS" in out or b"self-test: FAIL" in out) and \
               (cmd.encode() in out):
                # 再多收一点子项
                time.sleep(0.5)
                out += ser.read(2000)
                done = True
                break
    # PING 确认控制台存活
    ser.write(b"PING\r")
    tp = time.time()
    ping = b""
    while time.time() - tp < 3:
        c = ser.read(200)
        if c:
            ping += c
            if b"PONG" in ping:
                break
    ser.close()
    txt = out.decode(errors="replace")
    print("===== %s output =====" % cmd)
    for line in txt.splitlines():
        if "[INV]" in line or "[FUZZ]" in line or "self-test:" in line or "IRQ" in line:
            print(line.strip())
    alive = b"PONG" in ping
    print("CONSOLE_ALIVE:", alive)
    print("RESULT:", "PASS" if (b"self-test: PASS" in out and alive) else "FAIL/UNK")

if __name__ == "__main__":
    main()
