@echo off
taskkill /f /im openocd.exe
timeout /t 1
start "openocd_gdb" /min cmd /c ""D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe" -s "D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts" -f interface/stlink.cfg -f target/stm32f4x.cfg -c "reset_config srst_only connect_assert_srst" -c "init" -c "halt" < nul"
