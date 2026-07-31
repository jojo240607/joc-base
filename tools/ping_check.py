import serial, time, subprocess, sys

OCD = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
SCR = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"

# resume target in case halted
subprocess.run([OCD,"-s",SCR,"-f","interface/stlink.cfg","-f","target/stm32f4x.cfg",
                "-c","transport select swd","-c","init","-c","resume","-c","exit"],
               capture_output=True, text=True, timeout=30)

ser = serial.Serial(PORT, 115200, timeout=0.5)
ser.dtr = False; ser.rts = False
time.sleep(0.5)
ser.reset_input_buffer()
ser.write(b"PING\n")
time.sleep(1.0)
resp = ser.read(200)
ser.close()
print("[ping] response:", resp)
print("[ping] PONG" if b"PONG" in resp else "[ping] NO PONG (board unresponsive)")
