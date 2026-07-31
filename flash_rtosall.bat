@echo off
REM flash_rtosall.bat - flash build image and run RTOSALL invariant check
setlocal
set ELF=build/stm32f407_minimal.elf
set BIN=build/stm32f407_minimal.bin
set OCD="D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
if not exist %BIN% (
    echo [ERROR] %BIN% not found. Build first: cmake --build build
    exit /b 1
)
echo [*] Flashing %BIN% ...
%OCD% -s "D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts" -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program %BIN% verify reset exit 0x08000000"
if errorlevel 1 (
    echo [ERROR] OpenOCD flash failed
    exit /b 1
)
echo [*] Flashed. Waiting for board to enumerate + boot BIST...
timeout /t 4 >nul
echo [*] Running RTOSALL + checking g_sched_invariant_fail ...
python tools\rtosall_check.py %1 %2 %3 %4
exit /b %errorlevel%
