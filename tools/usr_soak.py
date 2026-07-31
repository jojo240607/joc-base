#!/usr/bin/env python3
"""RTOSUSR 多轮 soak：非特权任务经 SVC 门反复进出，逼出潜在的调度器/临界区竞态
（CONTROL.nPRIV=1 -> rtos_need_svc -> SVC_Handler -> rtos_svc_dispatch）。

每轮发 RTOSUSR -> 捕获 -> 解析 RTOSUSR PASS/FAIL，随后 PING 确认控制台存活
（检测内核死锁/冻结）。任一轮 FAIL 或 PONG 丢失即停。
进度写入 usr_soak_summary.txt，详情写入 usr_soak.log。
"""
import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 12
BAUD = 115200
SUMMARY = "usr_soak_summary.txt"
RUN_WAIT = 14.0
PING_WAIT = 4.0

def open_port():
    s = serial.Serial(PORT, BAUD, timeout=0.5)
    s.dtr = False; s.rts = False
    return s

def send(s, c, wait):
    s.reset_input_buffer()
    s.write(c.encode())
    buf = bytearray(); end = time.time() + wait
    while time.time() < end:
        n = s.in_waiting
        if n: buf += s.read(n)
        time.sleep(0.02)
    return buf.decode(errors='replace')

def main():
    with open(SUMMARY, "w") as sf:
        sf.write(f"# RTOSUSR soak PORT={PORT} N={N} start={time.strftime('%H:%M:%S')}\n")
    s = open_port()
    time.sleep(6.0); s.reset_input_buffer()
    s.write(b"PING\r\n"); time.sleep(2.0); s.read(s.in_waiting)
    for r in range(1, N + 1):
        out = send(s, "RTOSUSR\r\n", RUN_WAIT)
        all_pass = ("RTOSUSR PASS" in out) and ("FAIL" not in out)
        pong = send(s, "PING\r\n", PING_WAIT)
        alive = ("PONG" in pong)
        verdict = "PASS" if (all_pass and alive) else "FAIL"
        with open(SUMMARY, "a") as sf:
            sf.write(f"round {r:2d}: RTOSUSR={'PASS' if all_pass else 'FAIL'} console_alive={alive} -> {verdict}\n")
        print(f"round {r}/{N}: RTOSUSR={'PASS' if all_pass else 'FAIL'} console_alive={alive} -> {verdict}", flush=True)
        if not all_pass or not alive:
            print(f"  !! STOP on round {r}. Dump tail:", flush=True)
            print(out[-1500:], flush=True)
            print("PING resp:", pong[-300:], flush=True)
            break
    s.close()
    with open(SUMMARY, "a") as sf:
        sf.write(f"# end={time.strftime('%H:%M:%S')}\n")
    print("SOAK DONE", flush=True)

if __name__ == "__main__":
    main()
