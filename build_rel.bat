@echo off
REM Build RELEASE firmware (RTOS_SELFTEST=OFF) -> build_rel/stm32f407_minimal.bin / .elf
REM Usage: run build_rel.bat
REM Flash after build: flash_sys_rel.bat (system) and/or flash_app.bat (app)
REM 注入正式 Rust App（轨 A，libapp.a 由 joc-app-rust: cargo build --release 产出，不含 demo feature）
set RUST_LIB=%CD%\..\joc-app-rust\target\thumbv7em-none-eabihf\release\libapp.a
cmake -S . -B build_rel -G Ninja -DRTOS_SELFTEST=OFF -DRUST_APP_LIB=%RUST_LIB%
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
