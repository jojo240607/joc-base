@echo off
REM Flash the built firmware to the STM32F4 Discovery via ST-Link (SWD).
REM Primary : OpenOCD `program` command (interface/stlink.cfg + target/stm32f4x.cfg)
REM           -- no gdb involved, so it works even when stdin is a pipe.
REM Fallback: STM32CubeProgrammer (connect-under-reset)
REM
REM NOTE: If the MCU core is currently asleep / not examinable, OpenOCD reports
REM "Examination failed" and flashing fails. This is a board *state* issue, not a
REM config issue: press the black RESET button (or power-cycle) on the board to
REM wake the core, then re-run this script. The retry loop below keeps trying for
REM a few minutes, so you can press RESET while it is running. Wiring NRST from the
REM ST-Link to the MCU reset pin also fixes it permanently (connect-under-reset).
setlocal
set OPENOCD="D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe"
set OCD_SCRIPTS="D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts"
set ELF=build\stm32f407_minimal.elf
set BIN=build\stm32f407_minimal.bin
set CUBEPROG="D:\soft\ST\STM32CubeCLT_1.19.0\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
set TRIES=18
REM OpenOCD's `-c "program <path>"` parses the path as a TCL string, where '\'
REM is an escape char. Convert backslashes to forward slashes for the .bin path.
set BIN_OCD=%BIN:\=/%

if not exist %ELF% (
    echo [ERROR] %ELF% not found. Build first: cmake --build build
    exit /b 1
)

echo [1/2] Flashing via OpenOCD program (interface/stlink.cfg + target/stm32f4x.cfg)...
echo   connect-under-reset (srst_only) so it works even if the core is asleep.
set /a TRY=0
:retry
set /a TRY+=1
echo   --- attempt %TRY%/%TRIES% ---
taskkill /f /im openocd.exe >nul 2>&1
timeout /t 1 >nul
%OPENOCD% -s %OCD_SCRIPTS% -f interface/stlink.cfg -f target/stm32f4x.cfg -c "reset_config srst_only connect_assert_srst" -c "init" -c "reset halt" -c "program %BIN_OCD% verify reset exit 0x08000000" < nul > ocd.log 2>&1
findstr /L /i "Verified OK" ocd.log >nul
if not errorlevel 1 (
    echo [OK] Flashed %BIN% via OpenOCD
    goto :done
)
echo   [WARN] attempt %TRY%: flash failed. See ocd.log.
if %TRY% lss %TRIES% (
    timeout /t 3 >nul
    goto :retry
)
echo [WARN] OpenOCD could not flash after %TRIES% tries (see ocd.log).

echo [2/2] Falling back to STM32_Programmer_CLI (connect-under-reset)...
%CUBEPROG% -c port=SWD mode=UR freq=1000 -w %BIN% 0x08000000 -rst
if not errorlevel 1 (
    echo [OK] Flashed %BIN% via STM32_Programmer_CLI
    goto :done
)

echo [ERROR] Both flashers failed.
echo   The MCU core is currently not examinable (usually asleep / low-power).
echo   Fix: press the black RESET button on the board, then IMMEDIATELY re-run
echo   this script (or launch the VS Code cortex-debug launch). Wiring NRST to
echo   the ST-Link also makes connect-under-reset reliable.
exit /b 1

:done
taskkill /f /im openocd.exe >nul 2>&1
endlocal
exit /b 0
