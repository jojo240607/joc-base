import serial, time, sys
port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
cmds = sys.argv[2:] or ["RTOSMPU"]
ser = serial.Serial(port, 115200, timeout=0.5)
time.sleep(3.0)   # 等待板子 BIST 启动完成（开串口会脉冲复位，命令须等 boot 后再发）
ser.write(b"\r\n")  # wake
for c in cmds:
    ser.write((c + "\r\n").encode())
    print(">>", c)
    t0 = time.time() + 8
    while time.time() < t0:
        line = ser.readline().decode(errors='replace')
        if line:
            print(repr(line.rstrip()))
ser.close()
print("done")
