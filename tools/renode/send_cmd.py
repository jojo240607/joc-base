# -*- coding: utf-8 -*-
# send_cmd.py - send console command lines to the firmware via USART1 RX.
#
# Executed from a .resc script with `include @...` AFTER `start` + `sleep N`,
# so the firmware has booted before the first command arrives. The responses
# are captured by uart_capture.py's CharReceived hook and land in stdout.
import sys
from Antmicro import Renode

CMDS = ["PING", "TICKS", "ECHO hello-renode"]

uart = None
for entry in self.Machine.GetRegisteredPeripherals():
    if str(entry.Name) in ("usart1", "uart0"):
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
