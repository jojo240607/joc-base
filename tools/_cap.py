import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
f = open("tools/_t.txt", "w")
s = serial.Serial(PORT, 115200, timeout=0.5)
time.sleep(0.3)
s.reset_input_buffer()
s.write(b"\r\nBIST\r\n")
end = time.time() + 40
while time.time() < end:
    ln = s.read(300).decode(errors="replace")
    if ln:
        f.write(ln)
        f.flush()
f.write("\n---END---\n")
f.close()
