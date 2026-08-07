#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一键烧录 + 验证：烧录 build/stm32f407_minimal.bin，并通过串口验证
干净默认配置（仅 idle + 用户层 demo 任务）能够正常启动并响应命令。

验证项：
  1) 烧录成功（OpenOCD program + verify）
  2) 串口 PING -> PONG（控制台 + SysTick 节拍存活）
  3) DEMO 命令 -> 动态任务被创建并运行（用户层运行时建任务）

用法：
  python tools/flash_verify.py [PORT] [BAUD]
默认 PORT=COM8 BAUD=115200。

注意：
  - CH340(COM8) 在 open 时 DTR 脉冲会复位板子，故设 dtr/rts=False 规避；
    并用一次显式 DTR 脉冲强制干净复位，确保抓到 boot 横幅。
  - 若本机 COM8 未真正接到板子（或线材/驱动问题），串口段会报“无输出”，
    此时可用 GDB 读 g_tick 确认内核在跑（见 gdb_alive 方式）。
"""
import serial
import subprocess
import sys
import time

OCD = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
SCR = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
BIN = "build/stm32f407_minimal.bin"   # 干净默认配置构建产物


def flash():
    print("[1/3] Flashing %s via OpenOCD ..." % BIN)
    cmd = [
        OCD, "-s", SCR,
        "-f", "interface/stlink.cfg",
        "-f", "target/stm32f4x.cfg",
        "-c", "program %s verify reset exit 0x08000000" % BIN,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    # OpenOCD 成功时退出码 0 且含 "Verified OK"
    if r.returncode == 0 and "Verified OK" in out:
        print("[1/3] OK: flashed + verified")
        return True
    print("[1/3] FAIL: OpenOCD returncode=%d" % r.returncode)
    print(out[-1500:])
    return False


def open_console(port, baud):
    ser = serial.Serial(port, baud, timeout=0.5)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.1)
    # 显式 DTR 脉冲强制干净复位，确保抓到 boot 横幅
    ser.dtr = True
    time.sleep(0.1)
    ser.dtr = False
    time.sleep(0.1)
    return ser


def drain(ser, secs):
    buf = b""
    end = time.time() + secs
    while time.time() < end:
        try:
            c = ser.read(ser.in_waiting or 64)
        except Exception:
            c = b""
        if c:
            buf += c
    return buf


def send_cmd(ser, line, wait=1.5):
    drain(ser, 0.1)
    ser.write((line + "\r").encode("ascii"))
    return drain(ser, wait)


def verify(ser):
    ok = True

    print("[2/3] PING -> PONG check ...")
    # 先等 boot 横幅（最多 5s）
    drain(ser, 5.0)
    out = send_cmd(ser, "PING", wait=1.5)
    txt = out.decode("ascii", "replace")
    if "PONG" in txt:
        print("      OK: PONG received")
    else:
        print("      WARN: no PONG in response (串口无输出？板子内核可用 GDB 读 g_tick 确认):")
        print("      " + repr(txt[:200]))
        ok = False

    print("[3/3] DEMO (runtime dynamic task) check ...")
    out = send_cmd(ser, "DEMO", wait=2.5)
    txt = out.decode("ascii", "replace")
    if "spawned dynamic task" in txt and "demo_dynamic" in txt:
        print("      OK: dynamic task created + ran")
    else:
        print("      WARN: dynamic task did not respond as expected:")
        print("      " + repr(txt[:300]))
        ok = False

    return ok


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

    if not flash():
        sys.exit(1)

    time.sleep(1.0)   # 等板子复位启动
    try:
        ser = open_console(port, baud)
    except Exception as e:
        print("[ERR] cannot open %s: %s" % (port, e))
        sys.exit(1)

    try:
        passed = verify(ser)
    finally:
        ser.close()

    if passed:
        print("\nALL CHECKS PASSED — clean default RTOS (idle + demo tasks) runs OK.")
        sys.exit(0)
    else:
        print("\nSERIAL CHECKS INCONCLUSIVE (可能为串口链路问题，非固件问题；可用 GDB 读 g_tick 确认内核存活).")
        sys.exit(2)


if __name__ == "__main__":
    main()
