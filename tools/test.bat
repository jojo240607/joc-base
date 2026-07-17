@echo off
REM One-click: build -> flash -> PC companion self-test (BIST + PING/ECHO loopback).
REM
REM Usage:
REM   tools\test.bat [PORT] [BAUD]
REM     PORT  serial port of the USB-TTL wired to USART1 (default COM8)
REM     BAUD  default 115200
REM
REM Requires: arm-none-eabi-gcc on PATH (or build once via VS Code), pyserial
REM   (pip install pyserial), and ST-Link NRST wired to the MCU reset pin.
setlocal
cd /d "%~dp0.."

set PORT=%1
if "%PORT%"=="" set PORT=COM8
set BAUD=%2
if "%BAUD%"=="" set BAUD=115200

echo === [1/3] Build ===
cmake --build build -j4
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

echo === [2/3] Flash (OpenOCD connect-under-reset) ===
call tools\flash.bat
if errorlevel 1 (
    echo [ERROR] flash failed
    exit /b 1
)

echo === [3/3] Companion self-test (%PORT% @ %BAUD%) ===
python tools\companion_test.py %PORT% %BAUD%
if errorlevel 1 (
    echo [ERROR] companion self-test failed
    exit /b 1
)

echo === ALL GREEN: build + flash + self-test PASS ===
endlocal
exit /b 0
