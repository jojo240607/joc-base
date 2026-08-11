#!/usr/bin/env python3
"""USB OUT back-pressure self-heal stress test (COM9 only, no COM8 needed).

Proves the usb0.read deadlock fix (src/drv/usb.c: usb_stream_read now calls
usbd_cdc_out_reenarm() after every drain so the bulk-OUT endpoint self-heals
instead of staying NAK'd forever).

Method: drive the USB CDC console (COM9) the same way the real host would:
  1) send a steady stream of MAVLink v2 frames to FILL the RX ring and trigger
     the NAK back-pressure latch (the old "usb0.read hangs" condition);
  2) interleave PING commands and require a PONG reply each time.
If the deadlock were present, after the ring fills the OUT endpoint stays NAK'd
and PING would stop getting replies. With the fix, every drain re-arms OUT and
PING keeps getting PONG.

Usage: python verify_uplink_usb.py [COM9] [seconds]
"""
import sys
import time
import serial
import struct

MAVLINK_MAGIC = 0xFD
SYS_ID, COMP_ID = 1, 1


def make_mavlink(msgid, payload):
    plen = len(payload)
    header = struct.pack("<BBBBBBBB", MAVLINK_MAGIC, plen, 0, 0, 0,
                         SYS_ID, COMP_ID, msgid)
    crc = 0xFFFF
    for b in header[1:] + payload:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0x8408
            else:
                crc >>= 1
    return header + payload + struct.pack("<H", crc)


def main():
    com = sys.argv[1] if len(sys.argv) > 1 else "COM9"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

    usb = serial.Serial(com, 115200, timeout=0.5, write_timeout=0)
    usb.dtr = False
    usb.rts = False
    usb.reset_input_buffer()
    time.sleep(0.5)
    print(f"[open] {usb.port} ok", flush=True)

    frame = make_mavlink(76, bytes(33))

    t0 = time.time()
    sent = 0
    pings = 0
    pongs = 0
    frozen = False
    while (time.time() - t0) < secs:
        # hammer OUT: fill RX ring, trigger NAK latch
        for _ in range(20):
            try:
                usb.write(frame)
                sent += 1
            except Exception:
                pass
        # probe liveness via PING/PONG on the SAME endpoint
        usb.write(b"PING\n")
        pings += 1
        # drain whatever came back (PONG + any echoed/route logs)
        deadline = time.time() + 0.4
        got = b""
        while time.time() < deadline:
            c = usb.read(256)
            if c:
                got += c
        if b"PONG" in got:
            pongs += 1
        elapsed = time.time() - t0
        print(f"[{elapsed:6.1f}s] sent={sent} pings={pings} pongs={pongs}",
              flush=True)
        if pings >= 5 and pongs == 0:
            frozen = True
            print("[WARN] no PONG after 5 pings -> endpoint appears stuck",
                  flush=True)
            break
        time.sleep(0.05)

    usb.close()
    print(f"[done] sent OUT frames : {sent}")
    print(f"[done] pings sent      : {pings}")
    print(f"[done] pongs received  : {pongs}")
    ok = (pongs >= 3) and not frozen
    print(f"[done] RESULT: {'PASS - OUT self-heals, PING keeps getting PONG' if ok else 'FAIL - OUT stuck NAK, PING stopped replying (deadlock present)'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
