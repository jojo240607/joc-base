#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ostest_hil.py - jOS RTOS 硬件在环(HIL)自动跑测脚本（docs/ostest.md §10/§5/§14）。

自动化闭环：
  1) 可选烧录：用 OpenOCD（ST-Link SWD）把构建好的固件烧进板子；
  2) 开串口：等 boot BIST 结束进入命令循环；
  3) 发命令：默认 RTOSALL（运行全部编译期收集的自测，见 RTOS_SELFTEST_ADD）；
  4) 解析：抓取 [RESULT] <Case>: PASS|FAIL、[SELFTEST] <mod>: PASS|FAIL、
     [SELFTEST] ALL: PASS|FAIL，生成机器可读报告并以退出码 0/1 表示成败。

依赖：pyserial（`pip install pyserial`）；烧录需 OpenOCD 在 PATH 或经 --openocd 指定。

用法：
  # 烧录并跑 RTOSALL（默认 COM8 @115200）
  python tools/ostest_hil.py --flash

  # 不烧录，只解析（板子已有固件）
  python tools/ostest_hil.py

  # 跑单个命令、自定义端口/超时
  python tools/ostest_hil.py --port COM9 --cmd RTOSBASIC --timeout 60

  # 自定义 OpenOCD 与脚本路径
  python tools/ostest_hil.py --flash --openocd D:/soft/openocd/bin/openocd.exe \
        --ocd-scripts D:/soft/openocd/share/openocd/scripts \
        --bin build/stm32f407_minimal.bin

退出码：0 = 全部 PASS；非 0 = 存在 FAIL / 通信失败 / 无结果 / 烧录失败。
"""
import argparse
import os
import re
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write("ERROR: pyserial not installed (pip install pyserial)\n")
    sys.exit(2)


# 设备侧每行输出都带 log 前缀 "I/rtos: "；正则允许可选前缀（同 rtos_test_all.py）
LOGP        = r"(?:[VDIWEF]/\w+:\s+)?"
RESULT_RE   = re.compile(LOGP + r"\[RESULT\]\s+(\S+):\s*(PASS|FAIL)\s*$")
SELFTEST_RE = re.compile(LOGP + r"\[SELFTEST\]\s+(\S+):\s*(PASS|FAIL)\s*$")
ALL_RE      = re.compile(LOGP + r"\[SELFTEST\]\s+ALL:\s+(PASS|FAIL)\s*$")
REPLY_RE    = re.compile(LOGP + r"^(RTOS\w+)\s+(PASS|FAIL)\s*$")


def open_port(port, baud):
    ser = serial.Serial(port, baud, timeout=1.0)
    ser.dtr = False   # 避免 CH340 DTR/RTS 复位 STM32
    ser.rts = False
    time.sleep(2.0)   # 等板子 UART 起来
    return ser


def drain_boot(ser, cap=60):
    """读到串口静默若干次即认为 boot BIST 完成、进入命令循环。"""
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


def flash_firmware(args):
    """用 OpenOCD 经 ST-Link(SWD) 烧录 .bin。返回 True/False。"""
    if not args.bin:
        sys.stderr.write("[HIL] --flash 需要 --bin <firmware.bin>\n")
        return False
    if not os.path.exists(args.bin):
        sys.stderr.write("[HIL] firmware not found: %s\n" % args.bin)
        return False
    ocd = args.openocd or "openocd"
    scripts = args.ocd_scripts or "scripts"
    bin_ocd = args.bin.replace("\\", "/")
    cmd = [
        ocd, "-s", scripts,
        "-f", "interface/stlink.cfg",
        "-f", "target/stm32f4x.cfg",
        "-c", "reset_config srst_only connect_assert_srst",
        "-c", "init", "-c", "reset halt",
        "-c", "program %s verify reset exit 0x08000000" % bin_ocd,
    ]
    sys.stderr.write("[HIL] flashing %s ...\n" % args.bin)
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=180)
    except Exception as e:  # noqa: BLE001
        sys.stderr.write("[HIL] flash subprocess error: %s\n" % e)
        return False
    out = proc.stdout.decode(errors="replace")
    if "Verified OK" in out:
        sys.stderr.write("[HIL] flash OK\n")
        return True
    sys.stderr.write("[HIL] flash failed (see openocd output):\n%s\n" % out[-1500:])
    return False


def run_command(ser, cmd, timeout):
    """发送命令并收集输出，直到匹配到 [SELFTEST] ALL / 命令回显 / 超时。
    返回捕获的行列表。"""
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    lines = []
    t0 = time.time()
    while time.time() - t0 < timeout:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode(errors="replace").rstrip("\r\n")
        lines.append(line)
        if ALL_RE.match(line):
            break
        if REPLY_RE.match(line):
            break
    return lines


def parse(lines, cmd):
    cases, modules, overall = {}, {}, None
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


def report(title, cases, modules, overall):
    print("=" * 64)
    print(title)
    print("=" * 64)
    if modules:
        print("--- Modules ---")
        for n in sorted(modules):
            print("  %-16s %s" % (n, "PASS" if modules[n] else "FAIL"))
    if cases:
        npass = sum(1 for v in cases.values() if v)
        print("--- Cases (%d) ---" % len(cases))
        for n in sorted(cases):
            print("  %-30s %s" % (n, "PASS" if cases[n] else "FAIL"))
        print("  cases: %d/%d PASS" % (npass, len(cases)))
    if overall is not None:
        print("--- Overall ---")
        print("  %s" % ("PASS" if overall else "FAIL"))
    print("=" * 64)


def all_pass(cases, modules, overall):
    if overall is not None and not overall:
        return False
    if any(not v for v in modules.values()):
        return False
    if any(not v for v in cases.values()):
        return False
    if not modules and not cases:
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description="jOS RTOS HIL auto-runner")
    ap.add_argument("--port", default="COM8", help="serial port (default COM8)")
    ap.add_argument("--baud", type=int, default=115200, help="baud (default 115200)")
    ap.add_argument("--cmd", default="RTOSALL",
                    help="command to send (default RTOSALL)")
    ap.add_argument("--timeout", type=int, default=120,
                    help="per-run timeout seconds (default 120)")
    ap.add_argument("--flash", action="store_true", help="flash firmware before testing")
    ap.add_argument("--bin", default="build/stm32f407_minimal.bin",
                    help="firmware .bin to flash (with --flash)")
    ap.add_argument("--openocd", default=None, help="path to openocd executable")
    ap.add_argument("--ocd-scripts", default="scripts",
                    help="OpenOCD scripts dir (TCL target/interface cfgs)")
    ap.add_argument("--no-drain-boot", action="store_true",
                    help="skip waiting for boot BIST to finish")
    ap.add_argument("--log", default=None, help="optional file to dump raw console")
    args = ap.parse_args()

    if args.flash:
        if not flash_firmware(args):
            sys.stderr.write("[HIL] flashing failed; abort\n")
            sys.exit(1)

    try:
        ser = open_port(args.port, args.baud)
    except Exception as e:  # noqa: BLE001
        sys.stderr.write("ERROR opening %s: %s\n" % (args.port, e))
        sys.exit(1)

    if not args.no_drain_boot:
        print("[HIL] draining boot BIST ...")
        drain_boot(ser)

    print("[HIL] sending '%s' ..." % args.cmd)
    lines = run_command(ser, args.cmd, args.timeout)
    ser.close()

    if args.log:
        try:
            with open(args.log, "w") as f:
                f.write("\n".join(lines))
        except Exception as e:  # noqa: BLE001
            sys.stderr.write("[HIL] cannot write log: %s\n" % e)

    cases, modules, overall = parse(lines, args.cmd)
    report("HIL: %s" % args.cmd, cases, modules, overall)
    sys.exit(0 if all_pass(cases, modules, overall) else 1)


if __name__ == "__main__":
    main()
