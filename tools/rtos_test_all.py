#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rtos_test_all.py - 收集 jOS RTOS 自测结果，生成机器可读报告。

根据 docs/rtos-test-plan.md §5：固件侧自测打印两类机器可解析行——
  [SELFTEST] <module>: PASS|FAIL     （rtos_selftest_run_all 对每个模块汇总）
  [RESULT]   <Case>:   PASS|FAIL     （RTOS_TEST_RESULT 宏对每个子 Case 汇总）
控制台对单命令回显一行：
  <CMD> PASS|FAIL                      （selftest_reply）

用法:
  # 默认：发 RTOSALL，解析 [SELFTEST] 各模块 + [RESULT] 各子 Case
  python tools/rtos_test_all.py [--port COM8] [--baud 115200] [--timeout 90]

  # 按 §3 顺序逐个发 RTOS* 命令，分别解析（适合定位单个模块）
  python tools/rtos_test_all.py --per-cmd

  # 仅运行单个命令
  python tools/rtos_test_all.py --cmd RTOSBASIC

退出码: 0 = 全部 PASS；非 0 = 存在 FAIL / 通信失败 / 无结果。
"""
import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write("ERROR: pyserial not installed (pip install pyserial)\n")
    sys.exit(2)


# 设备侧每行输出都带 log 前缀 "I/rtos: "，正则需允许可选的该前缀。
LOGP        = r"(?:[VDIWEF]/\w+:\s+)?"
RESULT_RE   = re.compile(LOGP + r"\[RESULT\]\s+(\S+):\s*(PASS|FAIL)\s*$")
SELFTEST_RE = re.compile(LOGP + r"\[SELFTEST\]\s+(\S+):\s*(PASS|FAIL)\s*$")
ALL_RE      = re.compile(LOGP + r"\[SELFTEST\]\s+ALL:\s+(PASS|FAIL)\s*$")
REPLY_RE    = re.compile(LOGP + r"^(RTOS\w+)\s+(PASS|FAIL)\s*$")

# §3 执行顺序
MODULES = ["RTOSBASIC", "RTOSIPC2", "RTOSROBUST", "RTOSIPC", "RTOSP4",
           "RTOSUSR", "RTOSMPU", "RTOSFPU", "RTOSBH", "RTOSSTRESS",
           "RTOSRR", "RTOSBUS", "RTOSKOBJ"]


def open_port(port, baud):
    ser = serial.Serial(port, baud, timeout=1.0)
    # 避免 CH340 DTR/RTS 在 open() 时复位 STM32（否则每次连接都重启 BIST 并竞争时序）
    ser.dtr = False
    ser.rts = False
    # 复位后 UART 需一点时间才就绪；若立即发命令，字节会在外设初始化前被丢弃。
    # 等 2s 让板子 UART 起来（命令即便落在 BIST 期间也会被 RX 缓冲、BIST 后处理）。
    time.sleep(2.0)
    return ser


def send_cmd(ser, cmd):
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())


def read_until(ser, terminator_re, timeout):
    """读取行直到某行匹配 terminator_re 或超时；返回捕获的所有行。"""
    lines = []
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            raw = ser.readline()
        except Exception as e:
            sys.stderr.write("read error: %s\n" % e)
            break
        if not raw:
            continue
        line = raw.decode(errors="replace").rstrip("\r\n")
        lines.append(line)
        if terminator_re and terminator_re.match(line):
            break
    return lines


def run_command(ser, cmd, terminator_re, per_try=40, tries=3):
    """发送命令并读取直到匹配 terminator 或超时。首轮若恰在 BIST 期间、命令被丢弃，
    则重试若干次（板子已就绪后会正常响应）。返回捕获的行列表。"""
    lines = []
    for i in range(tries):
        send_cmd(ser, cmd)
        t0 = time.time()
        got = False
        while time.time() - t0 < per_try:
            try:
                raw = ser.readline()
            except Exception as e:
                sys.stderr.write("read error: %s\n" % e)
                break
            if not raw:
                continue
            line = raw.decode(errors="replace").rstrip("\r\n")
            lines.append(line)
            if terminator_re and terminator_re.match(line):
                got = True
                break
        if got:
            return lines
        sys.stderr.write("  (no response, retry %d/%d)\n" % (i + 1, tries))
        time.sleep(1.0)
    return lines


def parse_block(lines, cmd=None):
    """返回 (cases, modules, overall_all) 三个 dict/list。
       cases:   {name: bool}   来自 [RESULT]
       modules: {name: bool}   来自 [SELFTEST] <mod>（不含 ALL）
       overall: bool or None   来自 [SELFTEST] ALL 或单命令回显
    """
    cases = {}
    modules = {}
    overall = None
    for ln in lines:
        m = RESULT_RE.match(ln)
        if m:
            cases[m.group(1)] = (m.group(2) == "PASS")
            continue
        m = SELFTEST_RE.match(ln)
        if m and m.group(1) != "ALL":
            modules[m.group(1)] = (m.group(2) == "PASS")
            continue
        m = ALL_RE.match(ln)
        if m:
            overall = (m.group(1) == "PASS")
            continue
        if cmd:
            m = REPLY_RE.match(ln)
            if m:
                modules[m.group(1)] = (m.group(2) == "PASS")
                overall = (m.group(2) == "PASS")
    return cases, modules, overall


def print_report(title, cases, modules, overall):
    print("=" * 60)
    print(title)
    print("=" * 60)
    if modules:
        print("--- Modules ---")
        for name in sorted(modules):
            print("  %-14s %s" % (name, "PASS" if modules[name] else "FAIL"))
    if cases:
        print("--- Cases (%d) ---" % len(cases))
        npass = sum(1 for v in cases.values() if v)
        for name in sorted(cases):
            print("  %-26s %s" % (name, "PASS" if cases[name] else "FAIL"))
        print("  cases: %d/%d PASS" % (npass, len(cases)))
    if overall is not None:
        print("--- Overall ---")
        print("  %s" % ("PASS" if overall else "FAIL"))
    print("=" * 60)


def all_pass(cases, modules, overall):
    if overall is not None and not overall:
        return False
    if any(not v for v in modules.values()):
        return False
    if any(not v for v in cases.values()):
        return False
    # 至少要有结果
    if not modules and not cases:
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description="Collect jOS RTOS self-test results.")
    ap.add_argument("--port", default="COM8", help="serial port (default COM8)")
    ap.add_argument("--baud", type=int, default=115200, help="baud (default 115200)")
    ap.add_argument("--timeout", type=int, default=90, help="per-run timeout seconds")
    grp = ap.add_mutually_exclusive_group()
    grp.add_argument("--per-cmd", action="store_true",
                     help="run each RTOS* command individually (§3 order)")
    grp.add_argument("--cmd", metavar="NAME", help="run a single command, e.g. RTOSBASIC")
    args = ap.parse_args()

    try:
        ser = open_port(args.port, args.baud)
    except Exception as e:
        sys.stderr.write("ERROR opening %s: %s\n" % (args.port, e))
        return 2

    total_ok = True

    if args.cmd:
        lines = run_command(ser, args.cmd, REPLY_RE, per_try=args.timeout)
        cases, modules, overall = parse_block(lines, cmd=args.cmd)
        print_report("RTOS Test: %s" % args.cmd, cases, modules, overall)
        total_ok = all_pass(cases, modules, overall)

    elif args.per_cmd:
        agg_cases = {}
        agg_modules = {}
        for mod in MODULES:
            lines = run_command(ser, mod, REPLY_RE, per_try=args.timeout)
            cases, modules, overall = parse_block(lines, cmd=mod)
            ok = all_pass(cases, modules, overall)
            agg_modules[mod] = ok
            agg_cases.update(cases)
            print_report("RTOS Test: %s -> %s" % (mod, "PASS" if ok else "FAIL"),
                         cases, modules, overall)
            time.sleep(0.3)
        total_ok = all(v for v in agg_modules.values())
        print_report("RTOS Test: ALL (per-cmd)", agg_cases, agg_modules, total_ok)

    else:
        lines = run_command(ser, "RTOSALL", ALL_RE, per_try=args.timeout)
        cases, modules, overall = parse_block(lines)
        print_report("RTOS Test: RTOSALL", cases, modules, overall)
        total_ok = all_pass(cases, modules, overall)

    ser.close()
    print("RESULT: %s" % ("ALL PASS" if total_ok else "FAILURES PRESENT"))
    return 0 if total_ok else 1


if __name__ == "__main__":
    sys.exit(main())
