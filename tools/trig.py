import serial, time, sys
cmd = sys.argv[1] if len(sys.argv) > 1 else "RTOSSTRESS"
port = sys.argv[2] if len(sys.argv) > 2 else "COM8"
wait = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0
ser = serial.Serial(port, 115200, timeout=0.2)
ser.dtr = False; ser.rts = False
time.sleep(8.0)
ser.reset_input_buffer()
ser.write((cmd + "\n").encode())
print("%s sent, waiting %.0fs..." % (cmd, wait))
time.sleep(wait)
ser.close()
print("done")
