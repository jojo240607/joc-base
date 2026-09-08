# -*- coding: utf-8 -*-
# uart_capture.py - Renode Python hook (IronPython 2.7, executed via `include @...`)
#
# Attaches a CharReceived hook to the emulated USART1 (firmware console "uart0")
# and echoes every received byte to stdout, so a headless run can capture the
# firmware console output in the renode log file.
#
# Usage (inside a .resc or -e command):
#   include @d:/projects/mcu/os/joc-base/tools/renode/uart_capture.py
#
# The hook installs itself; it is a no-op when usart1 is not present.
import sys
from Antmicro import Renode

CAPTURE_FILE = r"d:\projects\mcu\os\joc-base\tools\renode\uart_capture.log"
_cf = open(CAPTURE_FILE, "w")

class UartCapture(object):
    def __init__(self, label):
        self.label = label
        self.buf = ""
    def __call__(self, b):
        self.buf += chr(b)
        sys.stdout.write(chr(b))
        sys.stdout.flush()
        try:
            _cf.write(chr(b))
            _cf.flush()
        except Exception:
            pass
        if b == 10:                       # keep a copy of the last line
            self.buf = ""

capture = None
for entry in self.Machine.GetRegisteredPeripherals():
    name = str(entry.Name)
    if name in ("usart1", "uart4", "uart5", "usart2", "usart3", "uart0"):
        try:
            uart = clr.Convert(entry.Peripheral, Renode.Peripherals.UART.IUART)
            capture = UartCapture(name)
            uart.CharReceived += capture
            sys.stdout.write("[CAPTURE] hooked %s (firmware console uart0 = USART1)\n" % name)
            sys.stdout.flush()
        except Exception as ex:
            sys.stdout.write("[CAPTURE] hook %s failed: %s\n" % (name, ex))
            sys.stdout.flush()

def uart_send_line(*args):
    """Send a command line to the firmware console (for interactive checks).
    Monitor invocation: uart_send_line PING   (or: uart_send_line ECHO hello)"""
    line = " ".join(str(a) for a in args)
    if capture is None:
        sys.stdout.write("[CAPTURE] no uart hooked; cannot send\n")
        return
    # re-resolve the peripheral (hooks are stateless across `include` reloads)
    for entry in self.Machine.GetRegisteredPeripherals():
        if str(entry.Name) in ("usart1", "uart0"):
            uart = clr.Convert(entry.Peripheral, Renode.Peripherals.UART.IUART)
            for ch in line + "\r":
                uart.WriteChar(ord(ch))
            sys.stdout.write("[CAPTURE] sent: %s\n" % line)
            sys.stdout.flush()
            return
