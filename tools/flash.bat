@echo off
REM Flash the built firmware to the STM32F4 Discovery via ST-Link (SWD).
REM Primary : STM32CubeProgrammer (connect-under-reset, 1 MHz SWD)
REM Fallback: OpenOCD (board/stm32f4discovery.cfg)
setlocal
set CUBEPROG="D:\soft\ST\STM32CubeCLT_1.19.0\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
set OPENOCD="D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
set OCD_SCRIPTS="D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
set BIN=build\stm32f407_minimal.bin

if not exist %BIN% (
    echo [ERROR] %BIN% not found. Build first: cmake --build build
    exit /b 1
)

echo [1/2] Trying STM32_Programmer_CLI (ST-Link, connect-under-reset, freq=1000)...
%CUBEPROG% -c port=SWD mode=UR freq=1000 -w %BIN% 0x08000000 -rst
if not errorlevel 1 (
    echo [OK] Flashed %BIN% via STM32_Programmer_CLI
    goto :done
)

echo [WARN] STM32_Programmer_CLI failed, falling back to OpenOCD...
echo [2/2] Flashing via OpenOCD (board/stm32f4discovery.cfg, connect-under-reset)...
%OPENOCD% -s %OCD_SCRIPTS% -f board/stm32f4discovery.cfg -c "adapter speed 1000" -c "reset_config srst_only connect_assert_srst" -c "program %BIN% verify reset exit 0x08000000"
if not errorlevel 1 (
    echo [OK] Flashed %BIN% via OpenOCD
    goto :done
)

echo [ERROR] Both flashers failed. Check ST-Link/SWD wiring:
echo         SWCLK -^> PA14, SWDIO -^> PA13, GND -^> GND, and NRST -^> reset.
exit /b 1

:done
endlocal
