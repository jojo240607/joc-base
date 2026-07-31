import serial, time, sys
port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
ser = serial.Serial(port, 115200, timeout=0.2)
ser.dtr = False; ser.rts = False
time.sleep(8.0)
ser.reset_input_buffer()
def send(s): ser.write((s+"\n").encode())
def drain(budget, quiet):
    buf=bytearray(); t0=time.time(); last=time.time()
    while True:
        d=ser.read(4096)
        if d: buf+=d; last=time.time()
        if time.time()-last>quiet: break
        if time.time()-t0>budget: break
    return bytes(buf)
send("")
time.sleep(1.0)
send("RTOSSTRESS")
t=time.time()
out=drain(40.0, 1.5)
print("STRESS bytes=%d elapsed=%.1f" % (len(out), time.time()-t))
print("STRESS TAIL:\n"+out.decode('utf-8','replace')[-800:])
send("PING")
t=time.time()
p=drain(10.0, 1.0)
print("PING bytes=%d elapsed=%.1f" % (len(p), time.time()-t))
print("PING OUT:\n"+p.decode('utf-8','replace'))
ser.close()
