#!/usr/bin/env python3
# Catch the FIRST entry into Default_Handler via breakpoint, dump LR/PC/SP + stack.
# Single persistent socket to OpenOCD telnet.
import socket, serial, time, sys

OCD_HOST, OCD_PORT = "localhost", 4444
PORT, BAUD = "COM8", 115200
BP = "0x08000718"
LOG = "bd_result.txt"

class OCD:
    def __init__(self):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.s.settimeout(15.0)
        self.s.connect((OCD_HOST, OCD_PORT))
    def cmd(self, c, wait=0.8):
        self.s.sendall((c + "\n").encode())
        buf = b""
        end = time.time() + wait
        while time.time() < end:
            try:
                d = self.s.recv(4096)
                if d: buf += d
            except socket.timeout:
                break
        return buf.decode("utf-8", "replace")
    def close(self):
        try: self.s.close()
        except: pass

def emit(s):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(s + "\n")

open(LOG, "w", encoding="utf-8").close()
o = OCD()
emit("reset: " + o.cmd("reset run", 1.2))
time.sleep(9.0)                      # wait BIST in python
emit("halt: " + o.cmd("halt", 1.0))
emit("bp: " + o.cmd("bp %s 2" % BP, 0.6))
emit("resume: " + o.cmd("resume", 0.6))

ser = serial.Serial(PORT, BAUD, timeout=0.2)
ser.dtr = False; ser.rts = False
time.sleep(0.3); ser.reset_input_buffer()
ser.write(b"RTOSROBUST\n")

hit = False
for i in range(80):
    out = o.cmd("poll", 0.4)
    if "halted" in out:
        hit = True
        break
    time.sleep(0.5)

if not hit:
    emit("BREAKPOINT NOT HIT within timeout")
else:
    emit("=== Default_Handler ENTERED (first time) ===")
    emit("pc: " + o.cmd("reg pc", 0.4))
    emit("lr: " + o.cmd("reg lr", 0.4))
    emit("sp: " + o.cmd("reg sp", 0.4))
    emit("--- main stack 0x10001c00..0x10002000 ---")
    emit(o.cmd("mdw 0x10001c00 256", 1.2))
o.close()
ser.close()
emit("DONE")
