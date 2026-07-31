import serial, time, sys
PORT=sys.argv[1] if len(sys.argv)>1 else "COM8"
ser=serial.Serial(PORT,115200,timeout=0.5); ser.dtr=False; ser.rts=False
time.sleep(6.0); ser.reset_input_buffer()
def cmd(c, wait=5.0):
    ser.reset_input_buffer(); ser.write(c.encode())
    buf=bytearray(); end=time.time()+wait
    while time.time()<end:
        n=ser.in_waiting
        if n: buf+=ser.read(n)
        time.sleep(0.02)
    txt=buf.decode(errors='replace')
    print(f"===== [{c.strip()}] =====")
    print(txt)
    sys.stdout.flush()
    return txt
# 完整的 RTOS 自测计划：先单个模块，再 RTOSALL 串联，最后 PING 确认控制台存活
cmd("RTOSMARATHON\n", 15.0)
cmd("RTOSSTRESS\n", 15.0)
cmd("RTOSIPC\n", 8.0)
cmd("RTOSFPU\n", 8.0)
cmd("RTOSMPU\n", 8.0)
cmd("RTOSTIMER\n", 8.0)
cmd("RTOSUSR\n", 10.0)
cmd("RTOSROBUST\n", 12.0)
cmd("PING\n", 3.0)
cmd("RTOSALL\n", 25.0)
cmd("PING\n", 3.0)
ser.close()
