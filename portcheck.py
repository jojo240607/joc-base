import socket
s = socket.socket()
s.settimeout(2)
try:
    s.connect(('127.0.0.1', 3333))
    print('OPEN')
    s.close()
except Exception as e:
    print('CLOSED', e)
