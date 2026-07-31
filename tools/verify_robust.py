import serial, time, sys
PORT=sys.argv[1] if len(sys.argv)>1 else "COM8"
ser=serial.Serial(PORT,115200,timeout=1.0); ser.dtr=False; ser.rts=False
time.sleep(6.0); ser.reset_input_buffer()
def cmd(c, wait=3.0):
    ser.reset_input_buffer(); ser.write(c.encode()); time.sleep(wait)
    r=ser.read(4000)
    print(f"===== [{c.strip()}] =====")
    print(r.decode(errors='replace'))
    return r
cmd("RTOSROBUST\n", 12.0)
cmd("PING\n", 3.0)
cmd("RTOSALL\n", 22.0)
cmd("PING\n", 3.0)
ser.close()
