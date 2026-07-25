import serial, time, sys
u = serial.Serial('COM8', 115200, timeout=1)
for i in range(3):
    u.write(b'USBSTAT\r\n')
    time.sleep(0.3)
    print('=== poll', i, '===')
    print(u.read(4000).decode(errors='replace'))
