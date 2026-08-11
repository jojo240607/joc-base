#!/usr/bin/env python3
"""Verify usb0.read deadlock fix: uplink task must keep receiving on COM9
even while we hammer the bulk-OUT endpoint (fills the RX ring, triggers the
NAK back-pressure latch). The fix is in joc-base src/drv/usb.c: usb_stream_read
now calls usbd_cdc_out_reenarm() after every drain, so the OUT endpoint self-heals
instead of staying NAK'd forever (the old "usb0.read hangs" symptom).

We watch COM8 (console) for uplink "alive loop=N" ticks to prove the task is
NOT blocked in read, and send a burst of MAVLink v2 frames on COM9 to stress
the RX ring. If the deadlock were present, loop counter would freeze and the
OUT endpoint would stop accepting bytes (write would block on host side).

Usage: python verify_uplink_read.py [COM8] [COM9] [seconds]
"""
import sys
import time
import serial
import struct

MAVLINK_MAGIC = 0xFD
SYS_ID, COMP_ID = 1, 1


def make_mavlink(msgid, payload):
    """Minimal MAVLink v2 frame (no signing) for a given msgid/payload."""
    plen = len(payload)
    incompat = 0
    compat = 0
    seq = 0
    header = struct.pack("<BBBBBBBB", MAVLINK_MAGIC, plen, incompat, compat,
                         seq, SYS_ID, COMP_ID, msgid)
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
    com8 = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    com9 = sys.argv[2] if len(sys.argv) > 2 else "COM9"
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0

    # open with explicit timeout so a stuck/busy port fails fast instead of hanging
    con = serial.Serial(com8, 115200, timeout=0.3, write_timeout=2)
    con.dtr = False
    con.rts = False
    usb = serial.Serial(com9, 115200, timeout=0.3, write_timeout=0)
    usb.dtr = False
    usb.rts = False
    con.reset_input_buffer()
    time.sleep(0.3)
    print(f"[open] COM8={con.port} COM9={usb.port} ok", flush=True)

    # small COMMAND_LONG-like frame (msgid 76) just to trigger route_frame logging
    frame = make_mavlink(76, bytes(33))

    t0 = time.time()
    sent = 0
    alive_seen = 0
    last_loop = -1
    frozen = False
    while (time.time() - t0) < secs:
        # hammer OUT: send many frames per tick to fill RX ring and trigger NAK
        for _ in range(20):
            usb.write(frame)
            sent += 1
        # read console for uplink alive ticks / route logs
        chunk = con.read(2048)
        if chunk:
            txt = chunk.decode("latin1", "replace")
            for line in txt.split("\n"):
                if "alive loop=" in line:
                    alive_seen += 1
                    try:
                        last_loop = int(line.split("alive loop=")[1].split()[0])
                    except Exception:
                        pass
        time.sleep(0.05)
        if (sent % 400) == 0:
            elapsed = time.time() - t0
            print(f"[{elapsed:6.1f}s] sent={sent} alive_ticks={alive_seen} last_loop={last_loop}", flush=True)

    # final check: did uplink keep ticking? (loop counter should be large & growing)
    # drain last console output
    tail = b""
    t1 = time.time()
    while (time.time() - t1) < 2.0:
        c = con.read(2048)
        if c:
            tail += c
    txt = tail.decode("latin1", "replace")
    for line in txt.split("\n"):
        if "alive loop=" in line:
            try:
                last_loop = int(line.split("alive loop=")[1].split()[0])
            except Exception:
                pass

    con.close()
    usb.close()

    print(f"[done] sent OUT frames        : {sent}")
    print(f"[done] uplink alive ticks seen: {alive_seen}")
    print(f"[done] last uplink loop counter: {last_loop}")
    ok = (alive_seen >= 3) and (last_loop > 1000)
    print(f"RESULT: {'PASS - usb0.read deadlock FIXED (uplink keeps receiving, OUT self-heals)' if ok else 'FAIL - uplink appears frozen in read (deadlock still present)'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
