#!/usr/bin/env python3
"""
PC-side companion test for the STM32F4 Discovery OOC firmware.

It opens the USB-TTL serial port (default COM8 @ 115200 8N1), waits for the
board's boot banner + SELF-TEST line, then verifies the USART TX/RX loopback
by exchanging PING/PONG and an ECHO command.

Usage:
    python tools/companion_test.py [PORT] [BAUD]

Requirements:
    pip install pyserial

Exit code: 0 = all pass, 1 = failure, 2 = missing dependency / cannot open port.
"""
import sys
import time
import re

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed. Run:  pip install pyserial")
    sys.exit(2)

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
TIMEOUT = 2.0


def main():
    print(f"[companion] opening {PORT} @ {BAUD} ...")
    try:
        ser = serial.Serial(PORT, BAUD, timeout=TIMEOUT)
    except Exception as e:
        print(f"[companion] FAILED to open {PORT}: {e}")
        sys.exit(1)

    time.sleep(0.5)
    ser.reset_input_buffer()

    results = {}

    # 1) trigger a (re)run of the on-board self-test and read its verdict.
    #    The firmware re-runs BIST on the "BIST" command, so this works no
    #    matter when we connect. Retry a few times because right after a flash
    #    the board may still be booting and the first command can be lost.
    bist_ok = False
    for attempt in range(3):
        ser.reset_input_buffer()
        ser.write(b"BIST\n")
        deadline = time.time() + 4
        while time.time() < deadline:
            line = ser.readline().decode(errors="replace").strip()
            if not line:
                continue
            print(f"  board> {line}")
            if line.startswith("SELF-TEST:"):
                bist_ok = "PASS" in line
                break
        if bist_ok:
            break
        time.sleep(1)
    results["bist"] = bist_ok
    if not bist_ok:
        print("[companion] ERROR: no SELF-TEST line received (board not responding?)")
        ser.close()
        sys.exit(1)

    def exchange(cmd, expect):
        ser.reset_input_buffer()
        ser.write((cmd + "\n").encode())
        d = time.time() + TIMEOUT
        while time.time() < d:
            line = ser.readline().decode(errors="replace").strip()
            if not line:
                continue
            print(f"  board> {line}")
            if line == cmd.strip():        # ignore the locally-echoed command
                continue
            if line == expect:
                return True
        return False

    results["ping"] = exchange("PING", "PONG")
    results["echo"] = exchange("ECHO hello-companion", "hello-companion")

    # 3) ADC: ask the board to sample PA0 and confirm a well-formed reading.
    adc_ok = False
    ser.reset_input_buffer()
    ser.write(b"ADC\n")
    d = time.time() + TIMEOUT
    while time.time() < d:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            continue
        print(f"  board> {line}")
        if line == "ADC":          # ignore the locally-echoed command
            continue
        if line.startswith("ADC "):
            m = re.search(r"raw=(\d+)", line)
            if m:
                raw = int(m.group(1))
                adc_ok = (0 <= raw <= 4095)
            break
    results["adc"] = adc_ok

    # 4) ADC channel switch: sample the internal temperature sensor (CH16),
    #    a stable on-chip reference, to prove channel re-programming works.
    adc_temp_ok = False
    ser.reset_input_buffer()
    ser.write(b"ADC 16\n")
    d = time.time() + TIMEOUT
    while time.time() < d:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            continue
        print(f"  board> {line}")
        if line == "ADC 16":       # ignore the locally-echoed command
            continue
        if line.startswith("ADC "):
            m = re.search(r"raw=(\d+)", line)
            if m:
                raw = int(m.group(1))
                adc_temp_ok = (400 <= raw <= 2000)   # ~0.3..1.6 V @ 3.3V
            break
    results["adc_temp"] = adc_temp_ok

    ser.close()

    print("\n[companion] RESULTS:")
    ok = True
    for k, v in results.items():
        print(f"  {k:6s}: {'PASS' if v else 'FAIL'}")
        ok = ok and v
    print(f"[companion] OVERALL: {'PASS' if ok else 'FAIL'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
