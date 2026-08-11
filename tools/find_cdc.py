#!/usr/bin/env python3
"""Find the working STM32 CDC-ACM port and verify PING/PONG + USBSTAT.

Tries COM1..COM20, opens each with a short timeout, sends PING and USBSTAT,
and reports which one answers. Usage:
    python tools/find_cdc.py
"""
import serial, time, sys

def try_port(p):
    try:
        s = serial.Serial(p, 115200, timeout=0.4)
    except Exception:
        return None
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
            return buf
        return False  # opened but no response
    except Exception as e:
        try:
            s.close()
        except Exception:
            pass
        return None

def main():
    for i in range(1, 21):
        p = f"COM{i}"
        r = try_port(p)
        if r is True or (isinstance(r, bytes) and len(r) > 0):
            print(f"[FOUND] {p}")
            sys.stdout.write(r.decode("utf-8", "replace"))
            print("\n--- end ---")
            return
        elif r is False:
            print(f"{p}: opens but no reply")
        # else: cannot open
    print("No responding CDC port found.")

if __name__ == "__main__":
    main()
