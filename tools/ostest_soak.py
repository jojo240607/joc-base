#!/usr/bin/env python3
"""RTOSALL 多轮回归 soak：专盯内核边界/竞态路径（ostest_* 模块：
TC-MTX/TASK/SEM/EVT/KERNEL-004..006 等）。

每轮发 RTOSALL -> 捕获完整输出 -> 解析 [SELFTEST] ALL: PASS/FAIL，
随后 PING 确认控制台仍存活（检测内核死锁/冻结）。任一轮 FAIL 或 PONG 丢失即停。
进度写入 ostest_soak_summary.txt（每行一轮），详情写入 ostest_soak.log。
"""
import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 8
BAUD = 115200
SUMMARY = "ostest_soak_summary.txt"
ALL_WAIT = 35.0   # RTOSALL 最长等待
PING_WAIT = 4.0

def open_port():
    s = serial.Serial(PORT, BAUD, timeout=0.5)
    s.dtr = False; s.rts = False   # 避免 DTR 脉冲复位板子
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
        sf.write(f"# RTOSALL soak PORT={PORT} N={N} start={time.strftime('%H:%M:%S')}\n")
    s = open_port()
    time.sleep(6.0); s.reset_input_buffer()
    s.write(b"PING\r\n"); time.sleep(2.0); s.read(s.in_waiting)
    passed = failed = 0
    for r in range(1, N + 1):
        out = send(s, "RTOSALL\r\n", ALL_WAIT)
        ok_all = ("[SELFTEST] ALL: PASS" in out) and ("FAIL" not in out.split("[SELFTEST]")[-1] if "[SELFTEST]" in out else True)
        # 更稳妥：检查是否有任意 [RESULT] ... FAIL 或模块 FAIL
        has_fail = ("FAIL" in out)
        all_pass = ("[SELFTEST] ALL: PASS" in out) and not has_fail
        # 控制台存活探测
        pong = send(s, "PING\r\n", PING_WAIT)
        alive = ("PONG" in pong)
        verdict = "PASS" if (all_pass and alive) else "FAIL"
        with open(SUMMARY, "a") as sf:
            sf.write(f"round {r:2d}: RTOSALL={'PASS' if all_pass else 'FAIL'} console_alive={alive} -> {verdict}\n")
        print(f"round {r}/{N}: RTOSALL={'PASS' if all_pass else 'FAIL'} console_alive={alive} -> {verdict}", flush=True)
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
