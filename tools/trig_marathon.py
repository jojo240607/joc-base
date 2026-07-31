import serial, time, sys
port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
ser = serial.Serial(port, 115200, timeout=0.2)
ser.dtr = False; ser.rts = False
time.sleep(8.0)   # 等 BIST + 命令循环
ser.reset_input_buffer()
ser.write(b"RTOSMARATHON\n")
print("RTOSMARATHON sent, waiting 6s for any fault...")
time.sleep(6.0)
ser.close()
print("done")
