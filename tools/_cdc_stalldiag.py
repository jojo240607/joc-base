import serial, threading, time, sys

cdc_port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
uart_port = sys.argv[2] if len(sys.argv) > 2 else "COM8"

cdc = serial.Serial(cdc_port, baudrate=115200, timeout=0.2)
cdc.write_timeout = 0
cdc.reset_input_buffer()
uart = serial.Serial(uart_port, baudrate=115200, timeout=0.5)

payload = bytes((i * 73 + 11) & 0xFF for i in range(262144))
collected = bytearray()
stop = False

def reader():
    while not stop:
        c = cdc.read(4096)
        if c:
            collected.extend(c)

rt = threading.Thread(target=reader, daemon=True)
rt.start()

def uart_poll():
    # send USBSTAT and print response
    uart.write(b"USBSTAT\r\n")
    time.sleep(0.2)
    out = uart.read(2000)
    sys.stderr.write("=== USBSTAT @ %.1fs ===\n" % (time.time() - t0))
    for ln in out.decode(errors="replace").splitlines():
        if "usb" in ln.lower() or "EP1" in ln or "line_coding" in ln:
            sys.stderr.write(ln + "\n")
    sys.stderr.flush()

t0 = time.time()
sent = 0
last_poll = 0
while sent < len(payload) and (time.time() - t0) < 8.0:
    n = cdc.write(payload[sent:sent + 4096])
    sent += n
    if time.time() - last_poll > 1.0:
        uart_poll()
        last_poll = time.time()
        sys.stderr.write("sent=%d collected=%d\n" % (sent, len(collected)))
        sys.stderr.flush()

stop = True
time.sleep(0.5)
uart_poll()
sys.stderr.write("FINAL sent=%d collected=%d\n" % (sent, len(collected)))
cdc.close(); uart.close()
