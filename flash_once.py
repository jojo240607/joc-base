import subprocess, sys

OCD = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
SCR = r"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
BIN = "build/stm32f407_minimal.bin"  # forward slashes: OpenOCD parses path as TCL

cmd = [OCD, "-s", SCR,
       "-f", "interface/stlink.cfg",
       "-f", "target/stm32f4x.cfg",
       "-c", "reset_config srst_only connect_assert_srst",
       "-c", "init",
       "-c", "reset halt",
       "-c", "program %s verify reset exit 0x08000000" % BIN]

r = subprocess.run(cmd, stdin=subprocess.DEVNULL,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
print(r.stdout)
sys.exit(0 if "Verified OK" in r.stdout else 1)
