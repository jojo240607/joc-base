#!/usr/bin/env python3
"""Validate USB soft re-enumeration (USBRST) via the UART console (COM8).

Flow:
  1. Open the UART console (COM8 by default), send USBRST to the board.
  2. The board does a soft USB disconnect+reconnect (DCTL.SDIS toggled),
     so Windows tears down + rebuilds the CDC-ACM instance.
  3. Wait a few seconds for the host to re-enumerate, then scan COM1..COM20
     for a port that answers PING / USBSTAT, and print the driver counters.

Usage:
    python tools/verify_usbrst.py [UART_PORT] [BAUD]
"""
import serial, time, sys

UART = sys.argv[1] if len(sys.argv) > 1 else "COM8"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200


def uart_send_cmd(port, cmd, wait=2.0):
    s = serial.Serial(port, BAUD, timeout=0.5)
    s.dtr = False
    s.rts = False
    time.sleep(0.3)
    s.write((cmd + "\r\n").encode())
    buf = b""
    t = time.time()
    while time.time() - t < wait:
        d = s.read(4096)
        if d:
            buf += d
    s.close()
    return buf


def scan_cdc():
    for i in range(1, 21):
        p = f"COM{i}"
        try:
            s = serial.Serial(p, 115200, timeout=0.4)
        except Exception:
            continue
        try:
            s.dtr = False
            s.rts = False
            time.sleep(0.3)
            s.write(b"PING\r\n")
            s.write(b"USBSTAT\r\n")
            buf = b""
            t = time.time()
            while time.time() - t < 1.5:
                d = s.read(4096)
                if d:
                    buf += d
            s.close()
            if b"PONG" in buf or b"connected" in buf:
                return p, buf
        except Exception:
            try:
                s.close()
            except Exception:
                pass
    return None, None


def main():
    print(f"[1] UART console {UART}: sending USBRST ...")
    out = uart_send_cmd(UART, "USBRST", wait=3.0)
    sys.stdout.write(out.decode("utf-8", "replace"))
    print("[2] waiting for host re-enumeration (6s) ...")
    time.sleep(6.0)
    print("[3] scanning COM1..COM20 for a responding CDC port ...")
    port, buf = scan_cdc()
    if port:
        print(f"[FOUND] {port}")
        sys.stdout.write(buf.decode("utf-8", "replace"))
        print("\n--- USBRST verify: PASS ---")
    else:
        print("No responding CDC port found after USBRST.")
        print("--- USBRST verify: FAIL (check Device Manager for a fresh CDC port) ---")


if __name__ == "__main__":
    main()
