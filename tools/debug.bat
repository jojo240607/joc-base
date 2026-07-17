@echo off
REM One-shot debug session:
REM   1. start ST-LINK GDB server in the background
REM   2. launch arm-none-eabi-gdb with tools/gdbinit.txt (connect, flash, run)
setlocal
set PATH=%PATH%;D:\soft\ST\STM32CubeCLT_1.19.0\GNU-tools-for-STM32\bin
set GDBSERVER="D:\soft\ST\STM32CubeCLT_1.19.0\STLink-gdb-server\bin\ST-LINK_gdbserver.exe"
set CUBEPROG="D:\soft\ST\STM32CubeCLT_1.19.0\STM32CubeProgrammer\bin"
set ELF=build\stm32f407_minimal.elf

if not exist %ELF% (
    echo [ERROR] %ELF% not found. Build first: cmake --build build
    exit /b 1
)

start "STLinkGDB" %GDBSERVER% -e -d -p 61234 -cp %CUBEPROG%
timeout /t 2 >nul
arm-none-eabi-gdb -x tools\gdbinit.txt %ELF%
endlocal
