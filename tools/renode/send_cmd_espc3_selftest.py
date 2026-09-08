# -*- coding: utf-8 -*-
# temp: send RTOS self-test commands for ESP32-C3 RISC-V port verification
import sys
from Antmicro import Renode

CMDS = [
    "RTOSALL",
]

uart = None
for entry in self.Machine.GetRegisteredPeripherals():
    if str(entry.Name) in ("usart1", "uart0"):
        uart = clr.Convert(entry.Peripheral, Renode.Peripherals.UART.IUART)
        break

if uart is None:
    sys.stdout.write("[SEND] uart0 not found, nothing sent\n")
else:
    for line in CMDS:
        for ch in line + "\r":
            uart.WriteChar(ord(ch))
        sys.stdout.write("[SEND] sent: %s\n" % line)
        sys.stdout.flush()
