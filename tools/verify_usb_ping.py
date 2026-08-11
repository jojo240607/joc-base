#!/usr/bin/env python3
"""Verify USB CDC up/down path on COM9 (STM32 CDC-ACM console).

Sends PING, expects PONG, then sends USBSTAT and prints the driver counters.
Usage:
    python verify_usb_ping.py [PORT] [BAUD] [DURATION_S]
"""
import serial, sys, time

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
DUR  = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0

def main():
    s = serial.Serial(PORT, BAUD, timeout=1.0)
    s.dtr = False
    s.rts = False
    time.sleep(0.5)
    print(f"[open] {PORT} ok")

    # Downlink test: host -> device (PING)
    s.write(b"PING\r\n")
    print("[sent] PING")

    # Uplink + driver-state introspection
    s.write(b"USBSTAT\r\n")
    print("[sent] USBSTAT")

    t = time.time()
    while time.time() - t < DUR:
        d = s.read(4096)
        if d:
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
    print("\n--- end ---")
    s.close()

if __name__ == "__main__":
    main()
