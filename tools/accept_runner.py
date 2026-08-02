#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""accept_runner.py - jOS 硬实时验收 + 稳定性 回归脚本。

循环执行：烧录 -> RTOSACCEPT -> RTOSALL -> RTOSFUZZ -> RTOSINV（各 N 次），
解析 [LATENCY]/[JITTER] 行做跨运行退化检测(DRIFT)，解析 [RESULT]/[SELFTEST] ALL
行决定成败。见 docs/rtos-acceptance-test-plan.md §2。

退出码：0 = 全 PASS 且无致命失败；非 0 = 存在 FAIL / 通信失败 / 烧录失败。
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

LOGP = r"(?:[VDIWEF]/\w+:\s+)?"
RESULT_RE = re.compile(LOGP + r"\[RESULT\]\s+(\S+):\s*(PASS|FAIL)\s*$")
SELFTEST_RE = re.compile(LOGP + r"\[SELFTEST\]\s+(\S+):\s*(PASS|FAIL)\s*$")
ALL_RE = re.compile(LOGP + r"\[SELFTEST\]\s+ALL:\s+(PASS|FAIL)\s*$")
REPLY_RE = re.compile(LOGP + r"^(RTOS\w+)\s+(PASS|FAIL)\s*$")
# [LATENCY] isr_wake irq=.. rsp=.. min=.. avg=.. max=.. (budget<..) PASS
LAT_RE = re.compile(LOGP + r"\[LATENCY\]\s+(\S+)\s+(\S+)\s*$")
JIT_RE = re.compile(LOGP + r"\[JITTER\]\s+(\S+)\s+(\S+)\s*$")


def open_port(port, baud):
    ser = serial.Serial(port, baud, timeout=1.0)
    ser.dtr = False
    ser.rts = False
    time.sleep(2.0)
    return ser


def drain_boot(ser, cap=60):
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
    ocd = args.openocd or "openocd"
    scripts = args.ocd_scripts or "scripts"
    bin_ocd = args.bin.replace("\\", "/")
    cmd = [ocd, "-s", scripts,
           "-f", "interface/stlink.cfg", "-f", "target/stm32f4x.cfg",
           "-c", "reset_config srst_only connect_assert_srst",
           "-c", "init", "-c", "reset halt",
           "-c", "program %s verify reset exit 0x08000000" % bin_ocd]
    sys.stderr.write("[RUN] flashing %s ...\n" % args.bin)
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=180)
    except Exception as e:  # noqa: BLE001
        sys.stderr.write("[RUN] flash error: %s\n" % e)
        return False
    out = proc.stdout.decode(errors="replace")
    if "Verified OK" in out:
        sys.stderr.write("[RUN] flash OK\n")
        return True
    sys.stderr.write("[RUN] flash failed:\n%s\n" % out[-1500:])
    return False


def run_command(ser, cmd, timeout):
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


def parse_latency(lines):
    """返回 {name: {field:val}} 用于跨运行对比。"""
    out = {}
    for ln in lines:
        m = LAT_RE.match(ln)
        if m:
            out[m.group(1)] = m.group(2)
        m = JIT_RE.match(ln)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def main():
    ap = argparse.ArgumentParser(description="jOS RTOS acceptance/soak regression runner")
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--repeat", type=int, default=5, help="循环次数")
    ap.add_argument("--timeout", type=int, default=180, help="单条命令超时秒")
    ap.add_argument("--flash", action="store_true")
    ap.add_argument("--bin", default="build/stm32f407_minimal.bin")
    ap.add_argument("--openocd", default=None)
    ap.add_argument("--ocd-scripts", default="scripts")
    ap.add_argument("--no-drain-boot", action="store_true")
    ap.add_argument("--drift-pct", type=int, default=10,
                    help="跨运行 max 延迟退化阈值%%（超则警告）")
    args = ap.parse_args()

    if args.flash and not flash_firmware(args):
        sys.exit(1)

    try:
        ser = open_port(args.port, args.baud)
    except Exception as e:  # noqa: BLE001
        sys.stderr.write("ERROR opening %s: %s\n" % (args.port, e))
        sys.exit(1)

    if not args.no_drain_boot:
        drain_boot(ser)

    baseline = None          # 首次运行的 latency 快照（基线）
    fail = False
    summary = []

    for it in range(args.repeat):
        sys.stderr.write("\n===== RUN %d/%d =====\n" % (it + 1, args.repeat))
        run_ok = True
        run_lat = {}
        for cmd in ("RTOSACCEPT", "RTOSALL", "RTOSFUZZ", "RTOSINV"):
            sys.stderr.write("[RUN] %s ...\n" % cmd)
            lines = run_command(ser, cmd, args.timeout)
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
                        overall = (m.group(1) == "PASS")
            run_lat.update(parse_latency(lines))
            if overall is not None and not overall:
                run_ok = False
            if any(not v for v in modules.values()):
                run_ok = False
            if any(not v for v in cases.values()):
                run_ok = False
            # 打印本命令结果概要
            npass = sum(1 for v in modules.values() if v)
            sys.stderr.write("      %s: modules=%d/%d overall=%s\n"
                             % (cmd, npass, len(modules), overall))
        if not run_ok:
            fail = True
            summary.append("RUN %d: FAIL" % (it + 1))
        else:
            summary.append("RUN %d: PASS" % (it + 1))

        # 跨运行退化检测
        if baseline is None:
            baseline = run_lat
        else:
            for k, v in run_lat.items():
                # 提取 max= 数值
                mm = re.search(r"max=(\d+)", v)
                bm = re.search(r"max=(\d+)", baseline.get(k, ""))
                if mm and bm:
                    base = int(bm.group(1))
                    cur = int(mm.group(1))
                    if base > 0:
                        drift = (cur - base) * 100 // base
                        if drift > args.drift_pct:
                            sys.stderr.write(
                                "  [DRIFT] %s max %d -> %d (%+d%%, >%d%%)\n"
                                % (k, base, cur, drift, args.drift_pct))
        # 注：基线只取第一次；后续每次对比第一次

    ser.close()
    print("=" * 64)
    print("ACCEPT RUNNER SUMMARY")
    print("=" * 64)
    for s in summary:
        print("  " + s)
    print("  baseline latencies: %s" % (baseline if baseline else "none"))
    print("=" * 64)
    sys.exit(0 if not fail else 1)


if __name__ == "__main__":
    main()
