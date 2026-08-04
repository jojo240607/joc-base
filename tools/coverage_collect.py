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
  （默认会先依次发一组 RTOS 自测命令覆盖内核路径，再发 RTOSCOV 导出；
   默认含 RTOSALL（实测在 linker 修复后的 coverage 构建下可正常跑完 ALL: PASS，
   不再挂死）。RTOSROBUST/RTOSMPU/RTOSUSR/RTOSP4 含故意故障/越权恢复，依赖精确指令
   地址，在 gcov 改布局下个别用例仍有 FAIL 风险，故默认跳过；RTOSMARATHON 为长跑
   测试不立即返回，也跳过——见 --precmd 说明）

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
    # 关键：CH340(COM8) 在 open 时会有 DTR 脉冲复位 STM32，导致板子重启动、RTOSCOV
    # 导出与一次全新 boot 竞争 → 抓到 108 字节的残帧。设 dtr/rts=False 规避，与
    # cap_raw.py / accept_runner.py / ostest_hil.py 等同款处理。
    ser = serial.Serial(args.port, args.baud, timeout=args.timeout,
                        dsrdtr=False, rtscts=False)
    ser.dtr = False
    ser.rts = False
    return ser


def reset_board(ser):
    """通过固件 RESET 命令做软件复位（CH340 的 DTR 未接 NRST，硬件 DTR 复位在本板无效），
    随后排空 boot BIST 直到静默，使命令循环就绪且 gcov 处于全新 boot 状态
    （保证 RTOSCOV 是首次 __gcov_dump 调用——二次调用只发 START+魔法字就停，
    产出 108 字节残帧 → 0 覆盖）。"""
    import time
    try:
        ser.write(b"RESET\r\n")
    except Exception:
        pass
    time.sleep(0.3)
    # 排空启动日志：读到连续 3 次空行（约 3s 静默）即认为 boot 完成、进入命令循环。
    cap = time.time() + 45
    silent = 0
    while time.time() < cap:
        try:
            line = ser.readline().decode(errors="replace").strip()
        except Exception:
            line = ""
        if line:
            silent = 0
            sys.stdout.write("  boot> " + line + "\n")
            sys.stdout.flush()
        else:
            silent += 1
            if silent >= 3:
                break
    try:
        ser.reset_input_buffer()
    except Exception:
        pass


def read_exact(stream, n):
    """从流中严格读满 n 字节（串行可能分片返回）。"""
    buf = b""
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            raise RuntimeError("stream closed / timeout before %d bytes" % n)
        buf += chunk
    return buf


def build_gcno_list(build_dir):
    """扫描 build 树里被 --coverage 插桩的 .gcno，返回 [(dir, base, checksum)] 列表。

    帧名（newlib gcov 在裸机下发的是 build 目标绝对路径，不可用于配对），故用 .gcno
    的单元校验和精确配对：.gcno 头部 [tag][version][checksum][0]（小端），校验和在
    偏移 8:12；.gcda 头部 [tag][version][stamp][checksum]，其 stamp 在偏移 8:12。
    本工具链中同一 TU 的 .gcno 校验和与 .gcda stamp 一致（见 _dumpinspect.py 的实测），
    故以「.gcda 偏移 8:12 的值」为键去查 .gcno 校验和表即可无歧义配对，免去暴力试配。

    注意 gcc 把 gcno 命名为「对象文件 basename」（如 rtos_accept.c.obj → rtos_accept.c.gcno），
    故 build 树里的 notes 文件是 rtos_accept.c.gcno，而非 rtos_accept.gcno。gcov 按同
    base 查找 <base>.gcno / <base>.gcda，所以 .gcda 也必须命名为 rtos_accept.c.gcda、
    并以 base=rtos_accept.c 调 gcov（切勿再剥 .c 后缀，否则 gcov 去找不存在的
    rtos_accept.gcno → 0%）。陈旧构建可能残留裸 base 的 .gcno（同名不同校验和），由调用方
    在 build 树清理掉；本函数如实收录所有候选，由 gcda stamp 选出真正匹配的那个。
    """
    import os
    import struct
    lst = []
    for root, _dirs, files in os.walk(build_dir):
        rel = os.path.relpath(root, build_dir).replace("\\", "/")
        if "src/rtos" not in rel and "src/osal" not in rel:
            continue
        for f in files:
            if not f.endswith(".gcno"):
                continue
            base = f[:-len(".gcno")]
            cs = 0
            try:
                b = open(os.path.join(root, f), "rb").read(12)
                if len(b) >= 12:
                    cs = struct.unpack("<I", b[8:12])[0]
            except Exception:
                pass
            lst.append((root, base, cs))
    return lst


def build_gcno_checksum_map(gcno_list):
    """校验和 -> (dir, base) 映射；同一校验和出现多次时保留首个。"""
    m = {}
    for d, base, cs in gcno_list:
        if cs and cs not in m:
            m[cs] = (d, base)
    return m


def gcda_match(gcno_cs_map, data):
    """用 .gcda 偏移 8:12 的单元标识去查 .gcno 校验和表，返回 (dir, base) 或 None。"""
    import struct
    if len(data) < 12:
        return None
    key = struct.unpack("<I", data[8:12])[0]
    return gcno_cs_map.get(key)


def run_gcov(gcov_exe, build_dir, gcda_files):
    """对每个 .gcda 跑 gcov，解析 [RESULT] 行汇总，返回 (per_file, overall)。"""
    per_file = []
    tot_exec = tot_lines = tot_br_exec = tot_br = 0
    for gcda in gcda_files:
        d = os.path.dirname(gcda)
        # gcc 把 notes/data 命名为「对象文件 basename」，即 <base>.gcno / <base>.gcda，
        # 其中 base 含 .c（如 rtos_accept.c、sched.c）。gcov 按同名 base 查找，故这里
        # 【只剥 .gcda 后缀、保留 .c】，以 base（如 sched.c）调 gcov，让它找到
        # sched.c.gcno / sched.c.gcda。若再剥 .c 去找 sched.gcno（不存在）→ 0%。
        fname = os.path.basename(gcda)
        if fname.endswith(".gcda"):
            fname = fname[:-len(".gcda")]
        base = fname
        try:
            # 关键：必须把【显式 .gcda 文件】传给 gcov（配合 -o .），不能只传裸 base。
            # 只传裸 base（gcov -b -c sched.c）时，若同目录残留旧构建的裸 base .gcda
            # （如 sched.gcda，与 sched.c.gcda 校验和不同），gcov 会误配 → 0%。
            # 显式指定 <file>.gcda 可无歧义配对对应 .gcno，得到真实覆盖率
            # （实测 sched.c.gcda：裸 base 0.00% vs 显式文件 94.41%）。
            out = subprocess.run(
                [gcov_exe, "-b", "-c", "-o", ".", gcda],
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
    ap.add_argument("--gcov",
                    default=r"D:/soft/ST/STM32CubeCLT_1.19.0/GNU-tools-for-STM32/bin/arm-none-eabi-gcov.exe",
                    help="gcov executable (default: the arm-none-eabi-gcov from the same "
                         "STM32CubeCLT toolchain that built the firmware — MUST match the "
                         "compiler's gcov format version, else gcov reports 'no functions found')")
    ap.add_argument("--timeout", type=float, default=5.0, help="serial read timeout (s)")
    ap.add_argument("--predelay", type=float, default=0.5,
                    help="open port 后、发 RTOSCOV 前的等待秒数")
    ap.add_argument("--precmd", default="RTOSALL",
                    help="发 RTOSCOV 前依次发送的命令（逗号分隔），让自测覆盖 RTOS 路径。"
                         "默认 RTOSALL（实测在 linker 修复后的 coverage 构建下可正常跑完 ALL: PASS，"
                         "覆盖度最高，含 rtos_accept 验收套件）。注意：RTOSROBUST / RTOSMPU / RTOSUSR "
                         "/ RTOSP4 含故意故障/越权恢复、依赖精确指令地址，在 gcov 改布局下个别用例"
                         "仍有 FAIL 风险，故默认跳过；RTOSMARATHON 为长跑测试不立即返回也跳过。"
                         "设为 '' 可完全跳过 precmd（仅采 BIST 覆盖）。")
    ap.add_argument("--precmd-timeout", type=float, default=60.0,
                    help="等待每个 --precmd 子命令完成标记（<cmd> PASS|FAIL）的秒数，超时则继续")
    ap.add_argument("--file", default=None,
                    help="离线模式：直接解析已捕获的 dump 文件（tools/_observe.py 等抓取），"
                         "跳过串口连接与 RTOSCOV；其余解析/落盘/gcov 流程不变")
    ap.add_argument("--save-raw", default=None,
                    help="把收到的原始 dump（含 START/END 标记）原样存到该文件，便于离线调试")
    args = ap.parse_args()
    if not args.file:
        if not args.port and not args.tcp:
            ap.error("need --port or --tcp")
    if not os.path.isdir(args.build):
        ap.error("build dir not found: %s" % args.build)

    gcno_list = build_gcno_list(args.build)
    gcno_cs_map = build_gcno_checksum_map(gcno_list)
    if not gcno_list:
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
        # 关键：每次抓取前【硬件复位板子】再排空 boot BIST，保证 __gcov_dump 在全新 boot 后
        # 第一次被调用（newlib 的 gcov 在首次 dump 后才填充计数；第二次调用会只发 START+魔法字
        # 就停，产出 108 字节残帧 → 0 覆盖）。DTR 脉冲复位 STM32，随后排空启动日志直到静默，
        # 与 companion_test.py 同款做法，确保命令循环已就绪、自测路径未被前一次 dump 污染。
        reset_board(stream)

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

    if args.save_raw:
        try:
            with open(args.save_raw, "wb") as f:
                f.write(dump)
            print("[*] raw dump saved to %s" % args.save_raw, flush=True)
        except Exception as e:
            print("[WARN] cannot save raw dump: %s" % e, file=sys.stderr)

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
    # 预先扫描所有 GCOV magic 的位置（含结束帧），流式帧的数据区就是「本帧头结束」到
    # 「下一 magic」之间的字节——裸机 newlib 的 gcov 不会把数据补齐到 64 边界后再发下一帧头，
    # 故不能再依赖 64 边界扫描（那会跳过非 64 对齐的数据尾部导致取到 0 字节）。直接用下一
    # magic 作为数据分隔是最稳健的做法（与 gcov_dump.c 的流式协议一致：dlen==0 时 data 直到
    # 下一 magic / 结束帧）。
    mag = struct.pack(">I", GCOV_MAGIC)
    magic_pos = []
    p = 0
    while True:
        p = core.find(mag, p)
        if p < 0:
            break
        magic_pos.append(p)
        p += 4
    print("[*] parsing %d dump bytes (%d frames) ..." % (n, len(magic_pos)))

    def frame_info(at):
        """返回 (nlen, name, dlen, header_end_off)。"""
        nl = struct.unpack(">H", core[at + 4:at + 6])[0]
        nm = core[at + 6:at + 6 + nl].decode("utf-8", "replace")
        dl = struct.unpack(">I", core[at + 6 + nl:at + 10 + nl])[0]
        he = at + 10 + nl
        return nl, nm, dl, he

    for idx, at in enumerate(magic_pos):
        # 结束帧（nlen==0）或头部越界的残帧：停。
        if at + 10 > n:
            break
        nlen, name, dlen, he = frame_info(at)
        if nlen == 0:
            break  # 结束帧
        # 下一 magic（或末尾）作为数据结束
        nxt = magic_pos[idx + 1] if idx + 1 < len(magic_pos) else n
        if dlen == 0:
            # 流式：数据从 he 到 nxt
            data = core[he:nxt]
        else:
            if he + dlen > nxt:
                print("[WARN] truncated .gcda (need %d, have %d) at frame %d; skip"
                      % (dlen, nxt - he, idx), file=sys.stderr)
                unmatched += 1
                continue
            data = core[he:he + dlen]
        # 固件 gcov_stage_flush 在旧版本会把末块用零补齐到 64 字节，这些零会混进 .gcda 数据区、
        # 使文件尾多出零字节导致 gcov 把计数器判成全 0。这里去掉尾部零填充（真实 .gcda 总是
        # 4 字节对齐、不会以随意零字节结尾），让 gcov 读到精确的计数器区。
        if data and data[-1] == 0:
            stripped = data.rstrip(b"\x00")
            if len(stripped) % 4 == 0 and len(stripped) > 0:
                data = stripped
        ent = gcda_match(gcno_cs_map, data)
        if not ent:
            print("[WARN] no .gcno match for frame %d (%d bytes); skip" % (idx, len(data)))
            unmatched += 1
            continue
        d, base = ent
        # gcc 把 notes 命名为「对象文件 basename」即 <base>.gcno（如 rtos_accept.c.gcno），
        # 故 .gcda 也必须命名为 <base>.gcda（保留 .c），并以 base=rtos_accept.c 调 gcov，
        # 否则 gcov 去找不存在的裸 base .gcno → "cannot open data file"（0%）。
        gcda_path = os.path.join(d, base + ".gcda")
        try:
            with open(gcda_path, "wb") as f:
                f.write(data)
        except Exception as e:
            print("[WARN] cannot write %s: %s" % (gcda_path, e), file=sys.stderr)
        gcda_files.append(gcda_path)
        matched += 1
        print("    recv %s (%d bytes) -> matched %s" % (base, len(data), os.path.join(d, base)))
    # 退回兼容旧变量名
    pos = n

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
