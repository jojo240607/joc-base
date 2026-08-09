import sys, serial, time, subprocess

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
baud = 115200
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 12

ser = serial.Serial(port, baud, timeout=0.2)
ser.dtr = False
ser.rts = False
time.sleep(0.2)
ser.reset_input_buffer()

# reset the board via OpenOCD (no exit so it doesn't disturb)
ocd = (
    '"D:\\soft\\openocd\\openocd-4e78563-i686-w64-mingw32\\bin\\openocd.exe"'
    ' -s "D:\\soft\\openocd\\openocd-4e78563-i686-w64-mingw32\\share\\openocd\\scripts"'
    ' -f interface/stlink.cfg -f target/stm32f4x.cfg'
    ' -c "init" -c "reset run" -c "sleep 200" -c "shutdown"'
)
subprocess.Popen(ocd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.3)

buf = b""
t0 = time.time()
while time.time() - t0 < secs:
    d = ser.read(4096)
    if d:
        buf += d
ser.close()
txt = buf.decode("utf-8", "replace")
print("TOTAL_BYTES", len(buf))
print(txt)
