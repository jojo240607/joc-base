@echo off
REM Flash RELEASE system ROM (build_rel/stm32f407_minimal.bin) to 0x08000000.
REM Does NOT touch the app partition (sector 7~).
REM Prereq: run build_rel.bat first.
if not exist build_rel\stm32f407_minimal.bin (
  echo [ERR] build_rel\stm32f407_minimal.bin missing. Run build_rel.bat first.
  exit /b 1
)
"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe" -s "D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts" -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program build_rel/stm32f407_minimal.bin 0x08000000 verify reset exit"
