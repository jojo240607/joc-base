import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
CMD = sys.argv[2] if len(sys.argv) > 2 else "UARTDMA"
RX4 = b"ABCD"  # exactly 4 bytes the board's bulk RX DMA expects

s = serial.Serial(PORT, 115200, timeout=0.5)
s.dtr = False
time.sleep(0.1)
s.dtr = True
time.sleep(9)             # let the board boot + BIST finish (BIST runs >2.5s)
s.reset_input_buffer()

# 1) send the command
s.write((CMD + "\r\n").encode())

# 2) read until the board prints RX-READY (it arms the bulk RX DMA right after),
#    then send the 4 RX bytes so they land on the already-armed RX DMA.
buf = b""
t = time.time() + 6
while time.time() < t:
    d = s.read(200)
    if d:
        buf += d
        sys.stdout.write(d.decode(errors="replace"))
        sys.stdout.flush()
        if b"RX-READY" in buf:
            break

# 3) host sends the 4 bytes now (board arms bulk RX right after the MARKER TX)
s.write(RX4)
time.sleep(0.2)

# 4) read the result
end = time.time() + 6
while time.time() < end:
    d = s.read(200)
    if d:
        sys.stdout.write(d.decode(errors="replace"))
        sys.stdout.flush()
        if b"UARTDMA RX" in d:
            break

print("\n---DONE---")
