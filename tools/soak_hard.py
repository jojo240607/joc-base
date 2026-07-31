#!/usr/bin/env python3
"""高压 soak：最大化调度器/临界区/SVC 门压力。
每轮：RTOSSTRESS(高频建删任务) + RTOSUSR(非特权 SVC 门 IPC)；每 4 轮再插 RTOSALL(全边界)。
全程探测 FAIL 与控制台存活(PING->PONG)。任一项 FAIL 或死控制台即停。
进度写入 soak_hard_summary.txt，详情写入 soak_hard.log。
"""
import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 30
BAUD = 115200
SUMMARY = "soak_hard_summary.txt"
W_STRESS, W_USR, W_ALL, W_PING = 16.0, 14.0, 30.0, 4.0

def open_port():
    s = serial.Serial(PORT, BAUD, timeout=0.5)
    s.dtr = False; s.rts = False
    return s

def send(s, c, wait):
    s.reset_input_buffer(); s.write(c.encode())
    buf = bytearray(); end = time.time() + wait
    while time.time() < end:
        n = s.in_waiting
        if n: buf += s.read(n)
        time.sleep(0.02)
    return buf.decode(errors='replace')

def main():
    with open(SUMMARY, "w") as sf:
        sf.write(f"# HARD soak PORT={PORT} N={N} start={time.strftime('%H:%M:%S')}\n")
    s = open_port(); time.sleep(6.0); s.reset_input_buffer()
    s.write(b"PING\r\n"); time.sleep(2.0); s.read(s.in_waiting)
    stop = False
    for r in range(1, N + 1):
        parts = []
        o1 = send(s, "RTOSSTRESS\r\n", W_STRESS)
        parts.append(("STRESS", "RTOSSTRESS PASS" in o1 and "FAIL" not in o1))
        o2 = send(s, "RTOSUSR\r\n", W_USR)
        parts.append(("USR", "RTOSUSR PASS" in o2 and "FAIL" not in o2))
        if r % 4 == 0:
            o3 = send(s, "RTOSALL\r\n", W_ALL)
            parts.append(("ALL", "[SELFTEST] ALL: PASS" in o3 and "FAIL" not in o3))
        pong = send(s, "PING\r\n", W_PING)
        alive = "PONG" in pong
        bad = [n for n, ok in parts if not ok] or (not alive)
        verdict = "PASS" if not bad else "FAIL"
        line = f"round {r:2d}: " + " ".join(f"{n}={'PASS' if ok else 'FAIL'}" for n, ok in parts) \
               + f" console_alive={alive} -> {verdict}"
        with open(SUMMARY, "a") as sf:
            sf.write(line + "\n")
        print(line, flush=True)
        if bad:
            print(f"  !! STOP on round {r}.", flush=True)
            for n, o in zip(["STRESS","USR","ALL"], [o1, o2, o3] if r % 4 == 0 else [o1, o2, ""]):
                print(f"--- {n} tail ---", o[-800:], flush=True)
            print("PING resp:", pong[-300:], flush=True)
            stop = True; break
    s.close()
    with open(SUMMARY, "a") as sf:
        sf.write(f"# end={time.strftime('%H:%M:%S')} stopped={stop}\n")
    print("SOAK DONE", flush=True)

if __name__ == "__main__":
    main()
