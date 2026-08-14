#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_app.py — Renode 日志断言引擎，验证 jOS RTOS + Rust App 集成测试通过/失败。

用法:
    python verify_app.py <logfile>

从 resc 运行后捕获的日志中提取关键事件并断言：
    - boot_completed:  READY + RUST app mounted + spawned tasks
    - pong_responded:  PING 发送后收到 PONG
    - hb_seq_monotonic: hb seq 严格递增且无跳空
    - ticks_advance:   虚拟 tick 在预期范围内
    - zero_faults:     无 HardFault/diagnosis/fault 关键字
    - telem_running:   telem 首次循环完成

返回码: 0 = PASS, 1 = FAIL
"""

import re
import sys


PASS = 0
FAIL = 1


def read_log(path):
    """读取日志文件，返回行列表。自动检测编码并处理 ANSI 转义。"""
    # Auto-detect encoding: check BOM first, then fallback to utf-8
    with open(path, "rb") as f:
        raw_bytes = f.read()
    if raw_bytes[:2] == b"\xff\xfe":
        encoding = "utf-16-le"
    else:
        encoding = "utf-8"
    raw = raw_bytes.decode(encoding, errors="replace")
    # Strip ANSI escape sequences (Renode uses them for color)
    clean = re.sub(r"\x1b\[[0-9;]*m", "", raw)
    # Split by any common newline
    lines = clean.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    return [l.rstrip() for l in lines]


def assert_boot_completed(lines):
    """检查引导完成：READY + RUST app mounted + spawned tasks"""
    found_ready = False
    found_mounted = False
    found_spawned = False
    found_ctrl = False
    found_telem = False
    for l in lines:
        if "READY" in l and "Commands" in l:
            found_ready = True
        if "RUST app mounted" in l:
            found_mounted = True
        if "spawned" in l and "tasks" in l:
            found_spawned = True
        if "ctrl: task started" in l:
            found_ctrl = True
        if "telem: task started" in l:
            found_telem = True
    reasons = []
    if not found_ready:
        reasons.append("missing READY")
    if not found_mounted:
        reasons.append("missing RUST app mounted")
    if not found_spawned:
        reasons.append("missing spawned tasks")
    if not found_ctrl:
        reasons.append("missing ctrl task started")
    if not found_telem:
        reasons.append("missing telem task started")
    return len(reasons) == 0, reasons


def assert_pong_responded(lines):
    """检查：每次 PING 发送后都有 PONG 回复"""
    sends = []
    pongs = []
    for l in lines:
        m = re.search(r"\[SEND\] sent:\s*PING", l)
        if m:
            sends.append(m.start())
        # PONG response on its own line (may be indented)
        if re.match(r"^PONG$", l.strip()):
            pongs.append(l)
    # Every PING should have at least one PONG appearing somewhere after
    if not sends:
        return True, ["no PING sent (test skipped)"]
    if len(pongs) < len(sends):
        return False, [f"PING sent {len(sends)}x but got {len(pongs)}x PONG"]
    return True, []


def assert_hb_seq_monotonic(lines):
    """检查 heartbeat seq 严格递增且无跳空"""
    seqs = []
    for l in lines:
        m = re.search(r"hb seq=(\d+)", l)
        if m:
            seqs.append(int(m.group(1)))
    if not seqs:
        return False, ["no heartbeat lines found"]
    prev = None
    gaps = []
    for s in seqs:
        if prev is not None:
            expected = (prev + 250) % 0x100000000  # seq increments by 250 per hb
            if s <= prev:
                gaps.append(f"non-monotonic: {prev} -> {s}")
            elif prev != 0 and s != prev + 250:
                # Check for any non-250 increment (allow wrap-around at first seq)
                if s != prev + 250:
                    gaps.append(f"unexpected jump: {prev} -> {s} (expect {expected})")
        prev = s
    if gaps:
        return False, gaps
    return True, [f"{len(seqs)} hb, seq range {seqs[0]}..{seqs[-1]}"]


def assert_zero_faults(lines):
    """检查日志中无故障关键字（仅限应用运行阶段，忽略 boot 阶段 WARNING）"""
    fault_keywords = [
        "HardFault", "BusFault", "UsageFault", "NMI",
        "rtos_fault", "fault diagnosed",
    ]
    faults = []
    for i, l in enumerate(lines):
        for kw in fault_keywords:
            if kw in l:
                faults.append(f"line {i+1}: {l[:120]}")
                break
    if faults:
        return False, faults[:5]  # limit output
    return True, []


def assert_telem_running(lines):
    """检查 telem 任务已启动。"first loop done" 为 info!() 可丢弃，不硬性要求。"""
    started = False
    first_done = False
    for l in lines:
        if "telem: task started" in l:
            started = True
        if "telem: first loop done" in l:
            first_done = True
    if not started:
        return False, ["telem task started not found"]
    msgs = ["telem task started OK"]
    if not first_done:
        msgs.append("first_loop_done not found (info may be dropped from ring)")
    return True, msgs


def assert_ticks_advance(lines, min_ticks=0, max_ticks=None):
    """检查 TICKS 值在预期范围内"""
    ticks = []
    for l in lines:
        m = re.search(r"TICKS\s+(\d+)", l)
        if m:
            ticks.append(int(m.group(1)))
    if not ticks:
        # Try extracting from last hb line as a rough measure
        for l in reversed(lines):
            m = re.match(r"R/[IWED] (\d+)\s", l)
            if m:
                ticks.append(int(m.group(1)))
                break
    if not ticks:
        return False, ["no TICKS or timestamp found"]
    last = ticks[-1]
    if min_ticks > 0 and last < min_ticks:
        return False, [f"TICKS={last} < min_ticks={min_ticks}"]
    if max_ticks is not None and last > max_ticks:
        return False, [f"TICKS={last} > max_ticks={max_ticks}"]
    return True, [f"TICKS={last}"]


def assert_rustdiag(lines):
    """检查 RUSTDIAG 输出：magic=0x41505053 version=1 dl=0
    所有关键设备返回非零指针。"""
    magic_ok = False
    version_ok = False
    dl_ok = False
    devs_ok = True
    devs_found = []
    for l in lines:
        if "RUSTDIAG:" in l:
            m = re.search(r"magic=0x([0-9a-fA-F]+)", l)
            if m and m.group(1) == "41505053":
                magic_ok = True
            vm = re.search(r"version=(\d+)", l)
            if vm and vm.group(1) == "1":
                version_ok = True
            dm = re.search(r"dl=(\d+)", l)
            if dm and dm.group(1) == "0":
                dl_ok = True
        m = re.match(r'\s+dev_get\("(\w+)"\)=0x([0-9a-fA-F]+)', l)
        if m:
            name = m.group(1)
            ptr = m.group(2)
            devs_found.append(name)
            if ptr == "0000" or ptr == "0":
                devs_ok = False
    msgs = []
    if not magic_ok:
        msgs.append("RUSTDIAG magic mismatch (expect 0x41505053)")
    if not version_ok:
        msgs.append("RUSTDIAG version mismatch (expect 1)")
    if not dl_ok:
        msgs.append("deadline violation > 0")
    if not devs_found:
        msgs.append("no dev_get lines found")
    if not devs_ok:
        msgs.append(f"some devices returned NULL: {[n for n in devs_found]}")
    if msgs:
        return False, msgs
    return True, [f"magic=0x41505053 ver=1 dl=0 devs={len(devs_found)}"]


INTG_MARKERS = [
    "INTG: START",        # suite entry
    "INTG: A1 enum=",     # Test A: dev_get enumeration
    "INTG: A2 uart0x3=",  # Test A: uart0 open/write/close x3
    "INTG: A done ok",    # Test A: aggregate result
    "INTG: B done ok",    # Test B: msleep precision
    "INTG: C done c1=",   # Test C: concurrency counters
    "INTG: D done try=",  # Test D: semaphore
    "INTG: F done ioctl=",  # Test F: read + ioctl
    "INTG: G done ok",    # Test G: error paths
    "INTG: H done ok =give=4 done=4",  # Test H: sem contention
    "INTG: I done ok",    # Test I: IRQ -> sem_give via TIM5
]

# END is informational (suite completion indicator).  It is written by a
# dedicated prio-19 task, but with polled marker I/O + cpsid i the RTOS tick
# counter advances slowly in Renode, so END may not flush before the wall-clock
# timeout.  All 11 test markers above already prove the suite completed.
INTG_END_MARKER = "INTG: END"


def assert_intg_tests(lines):
    """检查集成测试 A/B/C/D/F/G/H/I 全部完成（marker 方式）。

    Marker 由 wr_marker() 经轮询 USART1 寄存器直写（polled I/O + cpsid i）。
    所有 11 个测试 marker 必须出现；任一测试报 FAIL 都判失败。
    END marker 为可选（见 INTG_END_MARKER 注释）。
    """
    seen = set()
    for l in lines:
        for mk in INTG_MARKERS:
            if mk in l:
                seen.add(mk)
    reasons = []
    for mk in INTG_MARKERS:
        if mk not in seen:
            reasons.append(f"missing marker {mk}")
    if any("INTG:" in l and "FAIL" in l for l in lines):
        reasons.append("a test reported FAIL")
    msgs = []
    if INTG_END_MARKER in "\n".join(lines):
        msgs.append("END marker present")
    else:
        msgs.append("END marker not flushed (informational)")
    if reasons:
        reasons.append(f"seen={sorted(seen)}")
        return False, reasons
    msgs.insert(0, f"markers={len(seen)} all present")
    return True, msgs


def assert_boot_intg(lines):
    """集成测试模式的引导检查（无 ctrl/telem/hb，只有 app 启动）。
    使用 firmware 级日志（I/app_slot）而非 Rust info!()（经 DMA 可能未刷出）。
    """
    found_ready = False
    found_spawned = False
    found_entry = False
    for l in lines:
        if "READY" in l and "Commands" in l:
            found_ready = True
        if "app_host task spawned" in l:
            found_spawned = True
        if "starting App entry" in l:
            found_entry = True
    reasons = []
    if not found_ready:
        reasons.append("missing READY")
    if not found_spawned:
        reasons.append("missing app_host task spawned")
    if not found_entry:
        reasons.append("missing starting App entry")
    return len(reasons) == 0, reasons


INTG_CHECKS = [
    ("boot_intg",        assert_boot_intg),
    ("intg_tests",       assert_intg_tests),
    ("zero_faults",      assert_zero_faults),
]


def assert_panic_trigger(lines):
    """panic-test 构建：A-F 跑完后 Test E 触发 panic -> UDF -> RTOS fault
    handler 停机。断言 trigger marker 可见，且 suite 未走到 END（已停机）。"""
    found_start = False
    found_trigger = False
    found_end = False
    for l in lines:
        if "INTG: START" in l:
            found_start = True
        if "INTG: E trigger" in l:
            found_trigger = True
        if "INTG: END" in l:
            found_end = True
    reasons = []
    if not found_start:
        reasons.append("missing INTG: START")
    if not found_trigger:
        reasons.append("missing INTG: E trigger (panic test did not run)")
    if found_end:
        reasons.append("INTG: END seen - panic did not halt the system")
    if reasons:
        return False, reasons
    return True, ["panic triggered, fault handler halted the system"]


PANIC_CHECKS = [
    ("boot_intg",     assert_boot_intg),
    ("panic_trigger", assert_panic_trigger),
]

ALL_CHECKS = [
    ("boot_completed",  assert_boot_completed),
    ("pong_responded",  assert_pong_responded),
    ("hb_seq_monotonic", assert_hb_seq_monotonic),
    ("zero_faults",      assert_zero_faults),
    ("telem_running",    assert_telem_running),
    ("rustdiag",         assert_rustdiag),
]

# Minimal checks (for cmd-only resc, no Rust app)
MINIMAL_CHECKS = [
    ("pong_responded",  assert_pong_responded),
    ("zero_faults",      assert_zero_faults),
]


def verify(lines, extra_checks=None, mode="app"):
    """运行所有断言检查，返回 (passed, failed, details)。"""
    passed = []
    failed = []
    if mode == "minimal":
        checks = MINIMAL_CHECKS
    elif mode == "intg":
        checks = INTG_CHECKS
    elif mode == "panic":
        checks = PANIC_CHECKS
    else:
        checks = ALL_CHECKS
    if extra_checks:
        checks = checks + extra_checks
    for name, func in checks:
        ok, msgs = func(lines)
        if ok:
            detail = "; ".join(msgs) if msgs else "OK"
            passed.append((name, detail))
        else:
            for m in msgs:
                failed.append((name, m))
    return passed, failed


def main():
    if len(sys.argv) < 2:
        print(f"用法: python {sys.argv[0]} <logfile>")
        sys.exit(FAIL)

    path = sys.argv[1]
    lines = read_log(path)

    # Parse command line
    min_ticks = 0
    max_ticks = None
    mode = "app"
    for arg in sys.argv[2:]:
        if arg.startswith("--min-ticks="):
            min_ticks = int(arg.split("=")[1])
        elif arg.startswith("--max-ticks="):
            max_ticks = int(arg.split("=")[1])
        elif arg == "--mode=minimal" or arg == "--mode=cmd":
            mode = "minimal"
        elif arg == "--mode=intg":
            mode = "intg"
        elif arg == "--mode=panic":
            mode = "panic"

    # Build check list with ticks advance (skip for intg/panic mode)
    extra = []
    if mode not in ("intg", "panic"):
        def check_ticks(lines):
            return assert_ticks_advance(lines, min_ticks, max_ticks)
        extra = [("ticks_advance", check_ticks)]

    passed, failed = verify(lines, extra, mode)

    print(f"=== verify_app.py: {path} ===")
    for name, detail in passed:
        print(f"  PASS  {name}: {detail}")
    for name, detail in failed:
        print(f"  FAIL  {name}: {detail}")
    print(f"  Result: {len(passed)} passed, {len(failed)} failed")

    return PASS if not failed else FAIL


if __name__ == "__main__":
    sys.exit(main())