@echo off
REM Flash the built firmware to the STM32F4 Discovery via ST-Link (SWD)
setlocal
set CUBEPROG="D:\soft\ST\STM32CubeCLT_1.19.0\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
set BIN=build\stm32f407_minimal.bin

if not exist %BIN% (
    echo [ERROR] %BIN% not found. Build first: cmake --build build
    exit /b 1
)

%CUBEPROG% -c port=SWD -w %BIN% 0x08000000 -rst
if errorlevel 1 (
    echo [ERROR] Flashing failed. Check ST-Link connection.
    exit /b 1
)
echo [OK] Flashed %BIN%
endlocal
