#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cap_cmd.py - 发送单条控制台命令并捕获输出到文件/标准输出。

用法：
  python tools/cap_cmd.py --cmd RTOSACCEPT --secs 75 --out cap_accept.txt
  python tools/cap_cmd.py --cmd RTOSALL --secs 120

注：CH340(COM8) 在 open 时 DTR 脉冲会复位板子，已设 dtr/rts=False 规避。
"""
import argparse
import serial
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--cmd", default="RTOSALL")
    ap.add_argument("--secs", type=float, default=60)
    ap.add_argument("--out", default=None)
    ap.add_argument("--drain", action="store_true", help="先等 boot BIST 结束")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1.0)
    ser.dtr = False
    ser.rts = False
    time.sleep(2.0)

    if args.drain:
        silent = 0
        t0 = time.time() + 60
        while time.time() < t0:
            line = ser.readline().decode(errors="replace").strip()
            if line:
                silent = 0
            else:
                silent += 1
                if silent >= 3:
                    break
        ser.reset_input_buffer()

    ser.write((args.cmd + "\r\n").encode())
    lines = []
    outf = open(args.out, "w") if args.out else None
    t0 = time.time()
    while time.time() - t0 < args.secs:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode(errors="replace").rstrip("\r\n")
        lines.append(line)
        print(line)
        if outf:
            outf.write(line + "\n")
            outf.flush()
    ser.close()
    if outf:
        outf.close()


if __name__ == "__main__":
    main()
