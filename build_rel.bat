@echo off
REM Build RELEASE firmware (RTOS_SELFTEST=OFF) -> build_rel/stm32f407_minimal.bin / .elf
REM Usage: run build_rel.bat
REM Flash after build: flash_sys_rel.bat (system) and/or flash_app.bat (app)
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
