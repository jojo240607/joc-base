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

    # Drain the boot-time BIST (main.c runs selftest_run at startup). The BIST is
    # long andvariable in length, so instead of guessing a fixed sleep we drain
    # until the port goes SILENT (the command loop is idle waiting for input) —
    # that unambiguously means the boot run finished. Require 3 consecutive empty
    # reads (~3s of silence) so we don't stop early on a gap between BIST
    # sub-tests. Then clear the buffer and send OUR OWN BIST, which we can match
    # without racing the boot run.
    cap = time.time() + 45
    silent = 0
    while time.time() < cap:
        line = ser.readline().decode(errors="replace").strip()
        if line:
            silent = 0
        else:
            silent += 1
            if silent >= 3:
                break
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
        deadline = time.time() + 45   # timer BIST now exercises 14 timers + 2 shared-line tests
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

    # Settle: the board is back in its command loop right after SELF-TEST, but
    # give it a beat (and drain any trailing output) before firing interactive
    # commands, so PING/ECHO/ADC don't race the end of the (now ~5s) BIST.
    time.sleep(0.4)
    ser.reset_input_buffer()

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

    # 5) Temperature sensor driver: ask the board for the die temperature and
    #    confirm it reports a sane Celsius value via the dedicated driver.
    temp_ok = False
    ser.reset_input_buffer()
    ser.write(b"TEMP\n")
    d = time.time() + TIMEOUT
    while time.time() < d:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            continue
        print(f"  board> {line}")
        if line == "TEMP":          # ignore the locally-echoed command
            continue
        if line.startswith("TEMP "):
            m = re.search(r"C=(-?\d+)\.(\d+)", line)
            if m:
                temp = int(m.group(1)) + int(m.group(2)) / 10.0
                temp_ok = (-20.0 <= temp <= 120.0)
            break
    results["temp"] = temp_ok

    # 6) Interrupt framework: SysTick ISR increments a tick counter via the
    #    platform-independent irq framework. A non-zero count proves the shared
    #    ISR (IRQ_CommonHandler) dispatched a core exception and ran the callback.
    ticks_ok = False
    ser.reset_input_buffer()
    ser.write(b"TICKS\n")
    d = time.time() + TIMEOUT
    while time.time() < d:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            continue
        print(f"  board> {line}")
        if line == "TICKS":        # ignore the locally-echoed command
            continue
        if line.startswith("TICKS "):
            m = re.search(r"TICKS\s+(\d+)", line)
            if m and int(m.group(1)) > 0:
                ticks_ok = True
            break
    results["ticks"] = ticks_ok

    # 7) I2C IRQ mode: the handler takes ~5ms (two timeout-guarded transfers).
    #    Read all available data until we get the response or timeout.
    i2c_irq_ok = False
    time.sleep(0.1)                      # let any pending UART activity settle
    ser.reset_input_buffer()
    ser.write(b"I2C_IRQ\n")
    time.sleep(0.05)                     # wait for board to process + respond
    data = b""
    t = time.time() + TIMEOUT
    while time.time() < t:
        chunk = ser.read(256)
        if chunk:
            data += chunk
            decoded = data.decode(errors="replace")
            # print raw for diagnostics
            for line in decoded.split("\r\n"):
                stripped = line.strip()
                if stripped:
                    print(f"  board> {stripped}")
            # check if we have the full response
            if "I2C_IRQ:" in decoded and ("probe=NACK" in decoded or "open FAIL" in decoded or "no dev" in decoded):
                break
    decoded = data.decode(errors="replace")
    if "probe=NACK" in decoded and "xfer=0x50:0xA5=NACK" in decoded:
        i2c_irq_ok = True
    results["i2c_irq"] = i2c_irq_ok

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
