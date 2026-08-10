@echo off
REM Build RELEASE firmware (RTOS_SELFTEST=OFF) -> build_rel/stm32f407_minimal.bin / .elf
REM Usage: run build_rel.bat
REM Flash after build: flash_sys_rel.bat (system) and/or flash_app.bat (app)
REM
REM NOTE: This builds a PURE-C system firmware. It does NOT link libapp.a.
REM Deployment uses the partitioned/SPLIT flash layout (track B):
REM   - system area  (build_rel/*.bin) flashed to 0x08000000 by flash_sys_rel.bat
REM   - Rust app     (built independently in joc-app-rust -> app.bin) flashed to
REM     0x08060000 by flash_app.bat
REM App entry is discovered at boot by app_slot_boot.c from the partition header
REM (track-B branch), so libapp.a must NOT be linked into the system ELF.
REM (track-A single-ELF is for dev quick checks only; add -DRUST_APP_LIB=... manually)
cmake -S . -B build_rel -G Ninja -DRTOS_SELFTEST=OFF
if errorlevel 1 goto cmake_fail
cmake --build build_rel
if errorlevel 1 goto build_fail
echo [OK] release build done: build_rel/stm32f407_minimal.bin
goto :eof
:cmake_fail
echo [ERR] cmake configure failed
exit /b 1
:build_fail
echo [ERR] build failed
exit /b 1
