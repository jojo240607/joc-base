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
  （默认会先依次发一组“安全”的 RTOS 自测命令覆盖内核路径，再发 RTOSCOV 导出；
   RTOSALL/RTOSROBUST/RTOSMPU/RTOSUSR/RTOSP4 因覆盖率插桩改变代码布局，其故障/越权
   恢复依赖精确指令地址会挂死或 FAIL，故默认跳过——见 --precmd 说明）

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


def build_gcno_checksum_map(build_dir):
    """扫描 build 树里的 .gcno，建立 gcov 单元校验和 -> (目录, base) 的映射。

    裸机 newlib 的 gcov 无法推导每 TU 的真实路径，固件发出的 .gcda 帧名字全部相同
    （如 "build_cov\\CMakeFiles\\stm32f407_mini"），无法用文件名匹配。但 gcov 在
    .gcno 与 .gcda 里都嵌入了同一个「单元校验和」（magic "adcg"/"gcno" + version 之后
    的 4 字节，小端），用它即可把每个 .gcda 帧精确对应到其 .gcno。

    只收录被 --coverage 插桩的目录（src/rtos、src/osal），避免与非插桩 .gcno 的校验和
    撞车造成误配。返回 {checksum(int): (dir, base)}。
    """
    import struct
    mapping = {}
    for root, _dirs, files in os.walk(build_dir):
        rel = os.path.relpath(root, build_dir)
        if "src/rtos" not in rel.replace("\\", "/") and "src/osal" not in rel.replace("\\", "/"):
            continue
        for f in files:
            if not f.endswith(".gcno"):
                continue
            path = os.path.join(root, f)
            try:
                b = open(path, "rb").read(12)
            except Exception:
                continue
            if len(b) < 12:
                continue
            cs = struct.unpack("<I", b[8:12])[0]   # magic(4)+version(4)+checksum(4)
            base = f[:-len(".gcno")]
            mapping[cs] = (root, base)
    return mapping


def gcda_checksum(payload):
    """取 .gcda 帧数据里的单元校验和（payload[8:12]，小端）。"""
    import struct
    if len(payload) < 12:
        return None
    return struct.unpack("<I", payload[8:12])[0]


def run_gcov(gcov_exe, build_dir, gcda_files):
    """对每个 .gcda 跑 gcov，解析 [RESULT] 行汇总，返回 (per_file, overall)。"""
    per_file = []
    tot_exec = tot_lines = tot_br_exec = tot_br = 0
    for gcda in gcda_files:
        d = os.path.dirname(gcda)
        # gcda 文件名形如 "<base>.c.gcda"（newlib 的 gcov 把 TU 名按 .c 源文件命名）；
        # gcov 解析时按 "<base>.gcno / <base>.gcda" 查找，故必须【去掉 .c 后缀】
        # 得到裸 base 再传给 gcov（例：sched.c.gcda -> sched）。否则 gcov 会去找
        # sched.c.gcno（不存在）→ "cannot open notes file" → "No executable lines"。
        fname = os.path.basename(gcda)
        if fname.endswith(".gcda"):
            fname = fname[:-len(".gcda")]
        if fname.endswith(".c"):
            fname = fname[:-len(".c")]
        base = fname
        try:
            # gcov 必须 cwd 到含 .gcno/.gcda 的目录、并以裸 base 形式传入；
            # 传全路径 + -o <dir> 会让 gcov 误把目录名当对象名（找不到 .gcno）。
            out = subprocess.run(
                [gcov_exe, "-b", "-c", base],
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
    ap.add_argument("--precmd", default="RTOSBASIC,RTOSIPC,RTOSIPC2,RTOSSTRESS,RTOSFPU,"
                                         "RTOSBUS,RTOSRR,RTOSTIMER,RTOSBH",
                    help="发 RTOSCOV 前依次发送的命令（逗号分隔），让自测覆盖 RTOS 路径。"
                         "注意：RTOSALL / RTOSROBUST / RTOSMPU / RTOSUSR / RTOSP4 在覆盖率构建下"
                         "会因 gcov 改变代码布局而挂死或 FAIL（其故障/越权恢复依赖精确指令地址），"
                         "RTOSMARATHON 为长跑测试不立即返回，故默认跳过它们；设为 '' 可完全跳过。")
    ap.add_argument("--precmd-timeout", type=float, default=60.0,
                    help="等待每个 --precmd 子命令完成标记（<cmd> PASS|FAIL）的秒数，超时则继续")
    ap.add_argument("--file", default=None,
                    help="离线模式：直接解析已捕获的 dump 文件（tools/_observe.py 等抓取），"
                         "跳过串口连接与 RTOSCOV；其余解析/落盘/gcov 流程不变")
    args = ap.parse_args()
    if not args.file:
        if not args.port and not args.tcp:
            ap.error("need --port or --tcp")
    if not os.path.isdir(args.build):
        ap.error("build dir not found: %s" % args.build)

    gcno_map = build_gcno_checksum_map(args.build)
    if not gcno_map:
        print("[ERR] no .gcno found under %s; build with -DCOVERAGE=ON first" % args.build,
              file=sys.stderr)
        return 2

    end_marker = b"[GCOV DUMP END]"
    start_marker = b"[GCOV DUMP START]"

    if args.file:
        # 离线模式：直接解析已捕获的 dump 文件（tools/_observe.py 等抓取），
        # 跳过串口连接与 RTOSCOV；其余解析 / 落盘 / gcov 流程不变。
        with open(args.file, "rb") as f:
            dump = bytearray(f.read())
        print("[*] loaded dump from %s (%d bytes)" % (args.file, len(dump)), flush=True)
    else:
        import time
        stream = open_stream(args)
        try:
            stream.reset_input_buffer()
            time.sleep(0.5)   # 让 USB CDC 枚举 / DTR 握手稳定（与验证可用的裸机抓取脚本一致）
        except Exception:
            pass

        # 发 --precmd（逗号分隔的多个命令）先让自测覆盖 RTOS 路径，再发 RTOSCOV 触发导出。
        # 必须在发命令期间【轮询读取】每条命令的回显/结果——这会把 TX 环形缓冲排空，使随后
        # 的 .gcda 大块突发从「空且 64 字节对齐」的环形缓冲起步，避免 usb_tx_pump 被迫发出
        # 短包（短包会卡死 bulk_tx_pending，导致板子停在 ~16KB）。注意：发完 RTOSCOV 后【不】
        # 能 sleep（否则 OS 串口缓冲溢出丢字节），必须立刻进入下面的读取循环。
        buf = b""
        if args.precmd:
            cmds = [c.strip() for c in args.precmd.split(",") if c.strip()]
            for cmd in cmds:
                pre = (cmd + "\r\n").encode()
                try:
                    stream.write(pre)
                except Exception:
                    pass
                print("[*] sent precmd: %s" % cmd, flush=True)
                # 轮询直到看到完成标记 "<cmd> PASS|FAIL"，同时把 TX 环排空
                token = (cmd + " ").encode()
                deadline = time.time() + args.precmd_timeout
                done = False
                while time.time() < deadline:
                    chunk = stream.read(256)
                    if chunk:
                        buf += chunk if isinstance(chunk, bytes) else chunk.encode(errors="replace")
                        sys.stdout.buffer.write(buf[-256:])
                        sys.stdout.flush()
                        if (token + b"PASS") in buf or (token + b"FAIL") in buf:
                            done = True
                            break
                if not done:
                    print("\n[WARN] precmd %s did not report PASS/FAIL within %.0fs; "
                          "proceeding anyway" % (cmd, args.precmd_timeout), flush=True)
                buf = b""
            if cmds:
                print("[*] all precmds dispatched; sending RTOSCOV", flush=True)

        cmd = b"RTOSCOV\r\n"
        try:
            stream.write(cmd)
        except Exception:
            pass
        # 发完 RTOSCOV 后【绝不】 sleep，立刻进入下面的 START 等待循环持续读取，
        # 否则板子狂发 .gcda 帧时 OS 串口缓冲（~4KB）会溢出丢字节 → 帧错位。
        buf = b""   # START 探测从干净缓冲开始

        # 一次性把整段 dump 读进内存（持续 read，绝不在读取热路径里做磁盘 I/O），
        # 再离线解析帧。这与已验证可用的裸机抓取脚本同款读取模式：连续 read(2000) 直到
        # 见到 [GCOV DUMP END]（或静默 15s 视为结束）。读到 START/END 之间的二进制帧部分
        # 再离线切出，避免早期小粒度 read 干扰 USB 批量 IN 流控。
        dump = bytearray()
        deadline = time.time() + 120.0
        quiet_since = None
        last_rx = time.time()
        max_gap = 0.0
        n_chunks = 0
        n_bytes = 0
        print("[*] receiving full dump (reading into memory) ...", flush=True)
        while True:
            if end_marker in dump:
                break
            if time.time() > deadline:
                print("[ERR] timeout waiting for [GCOV DUMP END]", file=sys.stderr)
                return 2
            chunk = stream.read(2000)
            if chunk:
                now = time.time()
                gap = now - last_rx
                if gap > max_gap:
                    max_gap = gap
                last_rx = now
                n_chunks += 1
                n_bytes += len(chunk)
                dump += chunk if isinstance(chunk, bytes) else chunk.encode(errors="replace")
                quiet_since = None
            else:
                if quiet_since is None:
                    quiet_since = time.time()
                elif time.time() - quiet_since > 15.0:
                    print("[WARN] no new bytes for 15s; assuming dump ended without END marker",
                          file=sys.stderr)
                    break
        print("[*] read stats: chunks=%d bytes=%d max_gap=%.2fs" % (n_chunks, n_bytes, max_gap),
              flush=True)

    i = dump.find(start_marker)
    j = dump.find(end_marker)
    if i < 0:
        # 没有 START 标记：当作已经是「核心帧区」（tools/_observe.py 默认只存 core）。
        print("[*] [GCOV DUMP START] not in data; treating whole file as core",
              file=sys.stderr)
        core_start = 0
    else:
        core_start = i + len(start_marker)
    core = bytes(dump[core_start:j]) if j >= 0 else bytes(dump[core_start:])

    # 离线解析：从 core 顺序扫描每帧 magic + nlen + name + dlen + data。
    # 裸机 newlib 的 gcov 发不出真实 TU 名（帧名全是 build_cov\CMakeFiles\stm32f407_mini），
    # 用 .gcda 数据里的单元校验和（payload[8:12]）精确匹配到对应 .gcno。
    gcda_files = []
    matched = 0
    unmatched = 0
    pos = 0
    n = len(core)
    # START 标记串以 \n 结尾，core 起始可能带一个前导 \n；跳到第一段 GCOV magic 开始解析。
    first = core.find(struct.pack(">I", GCOV_MAGIC))
    if first > 0:
        core = core[first:]
        n = len(core)
    elif first < 0:
        print("[ERR] no GCOV magic found in dump", file=sys.stderr)
        return 2
    print("[*] parsing %d dump bytes ..." % n)
    while pos + 6 <= n:
        magic = struct.unpack(">I", core[pos:pos+4])[0]
        if magic != GCOV_MAGIC:
            # 已解析到若干完整帧后遇到坏 magic：通常是 dump 在被截断处之后的残留字节，
            # 属正常（板子在大块突发中途卡死），优雅收尾而不是整体 abort。
            if matched + unmatched > 0:
                print("[WARN] bad magic 0x%08X at off %d; stop (incomplete dump tail)"
                      % (magic, pos), file=sys.stderr)
                break
            print("[ERR] bad magic 0x%08X at off %d, abort" % (magic, pos), file=sys.stderr)
            return 2
        nlen = struct.unpack(">H", core[pos+4:pos+6])[0]
        pos += 6
        if nlen == 0:
            break  # 结束帧
        if pos + nlen + 4 > n:
            print("[WARN] truncated name/data header at off %d; stop (incomplete dump)"
                  % pos, file=sys.stderr)
            break
        name = core[pos:pos+nlen].decode("utf-8", "replace")
        pos += nlen
        dlen = struct.unpack(">I", core[pos:pos+4])[0]
        pos += 4
        if pos + dlen > n:
            print("[WARN] truncated .gcda data (need %d, have %d) at off %d; stop "
                  "(incomplete dump — board wedged mid-burst)"
                  % (dlen, n - pos, pos), file=sys.stderr)
            break
        data = core[pos:pos+dlen]
        pos += dlen
        # 整帧在线上补齐到 64 字节边界（见 gcov_dump.c），跳到下一帧的起点。
        pos = (pos + 63) & ~63
        cs = gcda_checksum(data)
        ent = gcno_map.get(cs) if cs is not None else None
        if not ent:
            print("[WARN] no .gcno match for checksum 0x%08X (frame name %r, %d bytes); skip"
                  % (cs if cs is not None else 0, name, dlen))
            unmatched += 1
            continue
        d, base = ent
        out_path = os.path.join(d, base + ".gcda")
        with open(out_path, "wb") as f:
            f.write(data)
        gcda_files.append(out_path)
        matched += 1
        print("    recv %s (%d bytes) -> %s" % (base, dlen, out_path))

    print("[*] received %d .gcda (matched=%d, unmatched=%d); running gcov ..."
          % (matched + unmatched, matched, unmatched))

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
