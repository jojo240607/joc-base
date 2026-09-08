# -*- coding: utf-8 -*-
# send_cmd_h7_rtosdma.py - send just RTOSDMA command.
import sys
from Antmicro import Renode

CMDS = ["PING", "RTOSDMA"]

uart = None
for entry in self.Machine.GetRegisteredPeripherals():
    if str(entry.Name) == "usart1":
        uart = clr.Convert(entry.Peripheral, Renode.Peripherals.UART.IUART)
        break

if uart is None:
    sys.stdout.write("[SEND] usart1 not found, nothing sent\n")
else:
    for line in CMDS:
        for ch in line + "\r":
            uart.WriteChar(ord(ch))
        sys.stdout.write("[SEND] sent: %s\n" % line)
        sys.stdout.flush()