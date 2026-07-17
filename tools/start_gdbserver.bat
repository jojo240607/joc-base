@echo off
REM Start the ST-LINK GDB server (SWD, GDB listens on TCP 61234).
REM Flash programming through gdb "load" uses STM32CubeProgrammer (-cp).
setlocal
set GDBSERVER="D:\soft\ST\STM32CubeCLT_1.19.0\STLink-gdb-server\bin\ST-LINK_gdbserver.exe"
set CUBEPROG="D:\soft\ST\STM32CubeCLT_1.19.0\STM32CubeProgrammer\bin"

%GDBSERVER% -e -d -p 61234 -cp %CUBEPROG%
endlocal
