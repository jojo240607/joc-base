#!/usr/bin/env python3
"""Verify STM32F4 CDC-ACM (VCP) TX/RX and characterize its baud-rate behavior.

KEY FACT: the CDC-ACM "baud rate" is VIRTUAL. The device stores the host's
SET_LINE_CODING value but does NOT time the wire by it -- the real data ceiling
is the USB FULL SPEED (12 Mbit/s) bulk endpoints. So the board accepts ANY host
baud rate (the value is just recorded). This script:

  1. auto-detects the CDC port (VID 0483 / PID 5740) or takes --port,
  2. sweeps several host baud rates and confirms the loopback echo still works
     (proves the device is baud-agnostic),
  3. runs a loopback throughput test (PC sends N bytes, the board echoes them
     back on the bulk-IN EP) and reports the achieved MB/s.

The board firmware echoes every byte it receives on the CDC bulk-OUT endpoint
back on the bulk-IN endpoint, so a matching echo proves BOTH directions:
  - RX: the board received exactly what the PC sent,
  - TX: the PC received exactly what the board sent.
"""
import argparse
import sys
import threading
import time

import serial
import serial.tools.list_ports

VID, PID = 0x0483, 0x5740


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == VID and p.pid == PID:
            return p.device
    return None


def echo_check(ser, tag, msg, timeout):
    ser.reset_input_buffer()
    ser.write(msg)
    got = b""
    t0 = time.time()
    while len(got) < len(msg) and (time.time() - t0) < timeout:
        got += ser.read(len(msg) - len(got))
    ok = got == msg
    print(f"  [{tag}] echo {'PASS' if ok else 'FAIL'} ({len(got)}/{len(msg)} bytes)")
    if not ok and got:
        print(f"      got={got!r}")
    return ok


def main():
    ap = argparse.ArgumentParser(description="STM32F4 CDC-ACM TX/RX + baud verify")
    ap.add_argument("--port", default=None, help="CDC COM port (auto-detect if omitted)")
    ap.add_argument("--bytes", type=int, default=262144, help="throughput payload size")
    ap.add_argument("--timeout", type=float, default=3.0)
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("ERROR: CDC-ACM port (0483:5740) not found.")
        print("Plug the board's CN5 (OTG FS) into THIS PC, wait for the COM port,")
        print("then re-run. Currently visible ports:")
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device}  vid={p.vid} pid={p.pid}  {p.description}")
        sys.exit(2)

    print(f"Opening {port} ...")
    ser = serial.Serial(port, baudrate=115200, timeout=args.timeout)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    time.sleep(0.2)

    # 1) echo sanity at default baud (proves RX+TX loopback)
    print("Echo sanity (RX+TX loopback):")
    echo_check(ser, "text", b"HELLO-CDC-ACM-loopback\r\n", args.timeout)

    # 2) baud sweep: set various host baud rates; device must still echo.
    #    All should PASS because CDC baud is virtual (device ignores it for timing).
    print("Baud-rate sweep (device is baud-agnostic; CDC baud is virtual):")
    bauds = [9600, 115200, 230400, 460800, 921600,
             1000000, 2000000, 3000000, 6000000, 12000000]
    for b in bauds:
        try:
            ser.baudrate = b
        except Exception as e:  # noqa: BLE001
            print(f"  {b:>10}: host setBaudrate failed ({e})")
            continue
        ser.reset_input_buffer()
        probe = b"BAUD%09d\r\n" % b
        ser.write(probe)
        got = b""
        t0 = time.time()
        while len(got) < len(probe) and (time.time() - t0) < args.timeout:
            got += ser.read(len(probe) - len(got))
        ok = got == probe
        print(f"  {b:>10} baud: echo {'PASS' if ok else 'FAIL'}")
    ser.baudrate = 115200

    # 3) throughput: PC sends N bytes, board echoes; measure RX throughput.
    print(f"Throughput loopback ({args.bytes} bytes):")
    payload = bytes((i * 73 + 11) & 0xFF for i in range(args.bytes))

    collected = bytearray()
    stop = False

    def reader():
        while not stop:
            chunk = ser.read(1024)
            if chunk:
                collected.extend(chunk)

    # Use NON-blocking writes: pyserial serializes read+write on one port lock,
    # and a blocking ser.write() would hold that lock while the OS waits for the
    # device to accept the (back-pressured) OUT packets -- starving the reader
    # thread and deadlocking the loopback. write_timeout=0 makes write() return
    # immediately with the count accepted by the OS, so the reader can drain IN.
    # Write in moderate (512 B) chunks: a single very large URB makes Windows'
    # usbser deprioritize the IN read pipe, which starves the echo and wedges
    # the loopback. 512 B still exercises sustained burst + back-pressure.
    ser.write_timeout = 0
    rt = threading.Thread(target=reader, daemon=True)
    rt.start()
    t0 = time.time()
    sent = 0
    while sent < len(payload):
        n = ser.write(payload[sent:sent + 512])
        sent += n
        if n == 0:
            # Yield so the reader thread (which shares pyserial's port lock) can
            # drain IN. Without this, the tight write loop starves the reader,
            # the host stops reading, and the device's back-pressure deadlocks.
            time.sleep(0.001)
    while len(collected) < len(payload) and (time.time() - t0) < args.timeout * 4:
        time.sleep(0.01)
    stop = True
    dt = time.time() - t0
    ok = bytes(collected[:len(payload)]) == payload
    mbps = (len(collected) / 1024 / 1024) / dt if dt > 0 else 0.0
    print(f"  echoed={len(collected)}/{len(payload)} bytes in {dt * 1000:.1f} ms "
          f"-> {mbps:.2f} MB/s")
    print(f"  integrity: {'PASS' if ok else 'FAIL'}")

    ser.close()
    print("DONE")


if __name__ == "__main__":
    main()
