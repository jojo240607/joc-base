#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""§6.6 代码覆盖率收集器（host 侧）。

配合固件的 -DCOVERAGE=ON 构建 + 控制台命令 RTOSCOV 使用：
  - 固件在 RTOSCOV 后把每个 .gcda 以 [magic|name_len|name|data_len|data] 二进制帧
    经调试 UART 透传，最后发一个 name_len==0 的结束帧。
  - 本脚本连上串口（或 QEMU 的 TCP 串口），发 "RTOSCOV"，收帧、按 basename 把
    .gcda 落到 build 树里对应的 .gcno 同目录，再调 gcov 出覆盖率报告。

用法：
  python tools/coverage_collect.py --port COM8 --build build_cov
  python tools/coverage_collect.py --tcp 127.0.0.1:1234 --build build_cov
  （先让板子跑完 BIST、并建议先敲 RTOSALL 跑一遍自测以覆盖 RTOS 代码路径）

依赖：pyserial（连 COM 时）。gcov 在 PATH（MinGW 自带 gcov.exe）。
"""
import argparse
import io
import os
import re
import struct
import subprocess
import sys

GCOV_MAGIC = 0x47434441  # "G C D A"


def open_stream(args):
    """返回一个类文件读对象（serial 或 TCP socket）。"""
    if args.tcp:
        import socket
        host, port = args.tcp.split(":")
        s = socket.create_connection((host, int(port)), timeout=10)
        return s.makefile("rwb", buffering=0)
    import serial  # 仅连 COM 时需要
    ser = serial.Serial(args.port, args.baud, timeout=args.timeout)
    return ser


def read_exact(stream, n):
    """从流中严格读满 n 字节（串行可能分片返回）。"""
    buf = b""
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            raise RuntimeError("stream closed / timeout before %d bytes" % n)
        buf += chunk
    return buf


def find_gcno_map(build_dir):
    """扫描 build 树里所有 .gcno，建立 basename(去扩展)->目录 的映射。"""
    mapping = {}
    for root, _dirs, files in os.walk(build_dir):
        for f in files:
            if f.endswith(".gcno"):
                base = f[:-len(".gcno")]  # 如 "sched"
                mapping[base] = root
    return mapping


def run_gcov(gcov_exe, build_dir, gcda_files):
    """对每个 .gcda 跑 gcov，解析 [RESULT] 行汇总，返回 (per_file, overall)。"""
    per_file = []
    tot_exec = tot_lines = tot_br_exec = tot_br = 0
    for gcda in gcda_files:
        d = os.path.dirname(gcda)
        base = os.path.splitext(os.path.basename(gcda))[0]
        try:
            out = subprocess.run(
                [gcov_exe, "-b", "-c", "-o", d, gcda],
                cwd=d, capture_output=True, text=True, timeout=60)
            txt = out.stdout + out.stderr
        except Exception as e:  # noqa: BLE001
            per_file.append((base, None, "gcov error: %s" % e))
            continue
        m = re.search(r"Lines executed:([\d.]+)% of (\d+)", txt)
        b = re.search(r"Taken at least once:([\d.]+)% of (\d+)", txt)
        if m:
            pct = float(m.group(1)); lines = int(m.group(2))
            tot_exec += int(round(lines * pct / 100.0)); tot_lines += lines
            br = ""
            if b:
                bp = float(b.group(1)); brn = int(b.group(2))
                tot_br_exec += int(round(brn * bp / 100.0)); tot_br += brn
                br = " branches %5.1f%% of %d" % (bp, brn)
            per_file.append((base, pct, "lines %5.1f%% of %d%s" % (pct, lines, br)))
        else:
            per_file.append((base, None, "no summary (see gcov output)"))
    overall = None
    if tot_lines:
        overall = (100.0 * tot_exec / tot_lines, tot_lines,
                   100.0 * tot_br_exec / tot_br if tot_br else 0.0, tot_br)
    return per_file, overall


def main():
    ap = argparse.ArgumentParser(description="Collect gcov .gcda over UART/TCP and report.")
    ap.add_argument("--port", help="serial port, e.g. COM8")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--tcp", help="QEMU serial as host:port, e.g. 127.0.0.1:1234")
    ap.add_argument("--build", default="build_cov", help="CMake build dir (holds .gcno)")
    ap.add_argument("--gcov", default="gcov", help="gcov executable (default: PATH gcov)")
    ap.add_argument("--timeout", type=float, default=5.0, help="serial read timeout (s)")
    ap.add_argument("--predelay", type=float, default=0.5,
                    help="open port 后、发 RTOSCOV 前的等待秒数")
    args = ap.parse_args()
    if not args.port and not args.tcp:
        ap.error("need --port or --tcp")
    if not os.path.isdir(args.build):
        ap.error("build dir not found: %s" % args.build)

    stream = open_stream(args)
    gcno_map = find_gcno_map(args.build)
    if not gcno_map:
        print("[ERR] no .gcno found under %s; build with -DCOVERAGE=ON first" % args.build,
              file=sys.stderr)
        return 2

    # 发 RTOSCOV 触发固件导出
    cmd = b"RTOSCOV\n"
    if isinstance(stream, io.RawIOBase) or hasattr(stream, "write"):
        stream.write(cmd)
    else:
        stream.write(cmd.encode())
    import time
    time.sleep(args.predelay)

    # 读到 [GCOV DUMP START] 之前都是普通文本，丢弃；START 之后的字节要保留，
    # 因为文本循环可能一次 read() 越过 START 读到二进制帧头，不能直接扔掉。
    buf = b""
    marker = b"[GCOV DUMP START]"
    print("[*] waiting for [GCOV DUMP START] ...")
    prefix = b""
    while marker not in buf:
        chunk = stream.read(128)
        if not chunk:
            print("[ERR] stream closed before dump start", file=sys.stderr)
            return 2
        buf += chunk if isinstance(chunk, bytes) else chunk.encode(errors="replace")
        if len(buf) > 8192:
            buf = buf[-8192:]
    prefix = buf[buf.find(marker) + len(marker):].lstrip(b"\r\n \t")

    class FrameReader:
        """先消费 prefix 残留字节，再继续从 stream 读，避免帧错位。"""
        def __init__(self, stream, init):
            self.stream = stream
            self.buf = bytearray(init)
        def read(self, n):
            while len(self.buf) < n:
                chunk = self.stream.read(256)
                if not chunk:
                    raise RuntimeError("stream closed mid-frame")
                self.buf += chunk if isinstance(chunk, bytes) else chunk.encode(errors="replace")
            out = bytes(self.buf[:n])
            del self.buf[:n]
            return out

    fr = FrameReader(stream, prefix)

    # 二进制帧模式
    gcda_files = []
    print("[*] receiving .gcda frames ...")
    while True:
        magic = struct.unpack(">I", fr.read(4))[0]
        if magic != GCOV_MAGIC:
            print("[ERR] bad magic 0x%08X, abort" % magic, file=sys.stderr)
            return 2
        nlen = struct.unpack(">H", fr.read(2))[0]
        if nlen == 0:
            break  # 结束帧
        name = fr.read(nlen).decode("utf-8", "replace")
        dlen = struct.unpack(">I", fr.read(4))[0]
        data = fr.read(dlen)
        base = name[:-len(".gcda")] if name.endswith(".gcda") else name
        d = gcno_map.get(base)
        if not d:
            print("[WARN] no .gcno for %s (basename %s), skip" % (name, base))
            continue
        out_path = os.path.join(d, name)
        with open(out_path, "wb") as f:
            f.write(data)
        gcda_files.append(out_path)
        print("    recv %s (%d bytes) -> %s" % (name, dlen, out_path))

    print("[*] received %d .gcda files; running gcov ..." % len(gcda_files))
    per_file, overall = run_gcov(args.gcov, args.build, gcda_files)

    # 打印 + 落盘报告
    lines_out = []
    lines_out.append("=== RTOS coverage (gcov) ===")
    for base, pct, info in sorted(per_file, key=lambda x: (x[1] is None, -(x[1] or 0))):
        lines_out.append("  %-22s %s" % (base, info))
    if overall:
        lines_out.append("  ------------------------------")
        lines_out.append("  OVERALL lines %5.1f%% of %d | branches %5.1f%% of %d"
                         % (overall[0], overall[1], overall[2], overall[3]))
    report = "\n".join(lines_out) + "\n"
    print(report)
    with open(os.path.join(args.build, "coverage_report.txt"), "w") as f:
        f.write(report)
    print("[*] report written to %s" % os.path.join(args.build, "coverage_report.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
