#!/usr/bin/env python3
# cap_log.py - 把串口(COM8)日志直接保存成文件
# 用法:
#   python cap_log.py                      # 默认 COM8, 抓 15 秒
#   python cap_log.py COM8 30              # 指定端口和时长(秒)
#   python cap_log.py COM8 0               # 时长 0 = 无限监听, 直到 Ctrl-C
#   python cap_log.py COM8 15 PING         # 启动后发 PING 触发回显
#   python cap_log.py COM8 35 --gdb-reset  # 用 GDB/OpenOCD 触发 ST-Link 硬件复位
#
# 日志写入: ./logs/<端口>_<时间戳>.log  (同时 stdout 也打印一份)
import serial, time, sys, os, subprocess

# 过滤掉 --gdb-reset 标志, 剩下位置参数才当作 port/dur/trig
args = [a for a in sys.argv[1:] if a != "--gdb-reset"]
port = args[0] if len(args) > 0 else "COM8"
dur  = float(args[1]) if len(args) > 1 else 15.0
trig = args[2] if len(args) > 2 else None
do_gdb_reset = "--gdb-reset" in sys.argv   # 用 GDB/OpenOCD 硬件复位(需 gdb 在 PATH)

OCD = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
OCD_SCRIPTS = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
GDB = "arm-none-eabi-gdb"

def gdb_hard_reset():
    """用 OpenOCD telnet(4444) 发 reset run 触发 ST-Link 硬件复位。
    不依赖 arm-none-eabi-gdb(不在 PATH), 直接用 OpenOCD 原生 telnet 接口。"""
    # 先确保没有残留 openocd
    subprocess.run(["taskkill", "/f", "/im", "openocd.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    # 后台起 openocd
    ocd = subprocess.Popen(
        [OCD, "-s", OCD_SCRIPTS, "-f", "interface/stlink.cfg",
         "-f", "target/stm32f4x.cfg", "-c", "gdb_port 3333",
         "-c", "tcl_port 6666", "-c", "telnet_port 4444"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(5.0)  # 等 openocd 连上 ST-Link + target
    # 通过 telnet 4444 发 reset run (OpenOCD 原生接口, 可靠)
    try:
        import socket
        s = socket.create_connection(("localhost", 4444), timeout=10)
        s.sendall(b"reset run\n")
        time.sleep(2.0)  # 等板子从 flash 启动 + BIST + 串口初始化
        s.sendall(b"exit\n")
        s.close()
    except Exception as e:
        print(f"[cap] telnet reset warn: {e}")
    return ocd

os.makedirs("logs", exist_ok=True)
stamp = time.strftime("%Y%m%d_%H%M%S")
logpath = os.path.join("logs", f"{port}_{stamp}.log")

# dsrdtr/rtscts=False 避免硬件流控
ser = serial.Serial(port, 115200, timeout=0.2, dsrdtr=False, rtscts=False)
# 打开后默认拉低 DTR(避免一开端口就误复位)
ser.dtr = False
ser.rts = False

# 先开串口监听, 再复位 —— 否则板子在监听前就重启完了抓不到开头
time.sleep(0.3)
ser.reset_input_buffer()

if do_gdb_reset:
    print("[cap] GDB hardware reset via OpenOCD/ST-Link...")
    gdb_hard_reset()

# 可选触发
if trig:
    time.sleep(0.5)
    ser.write((trig + "\r\n").encode())
    print(f"[cap] sent trigger: {trig}")
else:
    print("[cap] tip: 若板子没自动重启, 加 --gdb-reset 参数用 OpenOCD/ST-Link 硬件复位")

buf = bytearray()
t0 = time.time()
print(f"[cap] logging {port} -> {logpath}  (dur={dur}s, ctrl-c to stop)")

with open(logpath, "wb") as f:
    try:
        while True:
            if dur and (time.time() - t0 >= dur):
                break
            d = ser.read(400)
            if d:
                buf += d
                f.write(d)
                f.flush()          # 实时落盘, 断连也不丢
                sys.stdout.buffer.write(d)
                sys.stdout.buffer.flush()
    except KeyboardInterrupt:
        print("\n[cap] stopped by user")
    finally:
        # 收尾再读一点残留
        time.sleep(0.3)
        tail = ser.read(400)
        if tail:
            buf += tail
            f.write(tail)
            sys.stdout.buffer.write(tail)
        f.close()
        ser.close()

print(f"\n[cap] done. total {len(buf)} bytes -> {logpath}")
