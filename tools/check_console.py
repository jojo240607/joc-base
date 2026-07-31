import serial, time, sys
PORT=sys.argv[1] if len(sys.argv)>1 else "COM8"
ser=serial.Serial(PORT,115200,timeout=1.0); ser.dtr=False; ser.rts=False
time.sleep(10.0); ser.reset_input_buffer()
def cmd(c, wait=3.0):
    ser.reset_input_buffer(); ser.write(c.encode()); time.sleep(wait)
    r=ser.read(2000); print(f"[{c.strip():12s}] -> {r!r}"); return r
cmd("RTOSMARATHON\n", 3.0)
cmd("RTOSSTRESS\n", 12.0)     # heavy: give it time to finish printing
cmd("PING\n", 3.0)            # should recover
cmd("RTOSROBUST\n", 12.0)
cmd("PING\n", 3.0)
cmd("RTOSALL\n", 20.0)        # heaviest
cmd("PING\n", 3.0)
ser.close()
