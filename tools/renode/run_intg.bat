@echo off
REM run_intg.bat — 一键运行集成测试 (Layer 3)
REM
REM 用法:
REM   run_intg.bat           构建 Rust app + 运行 Renode + 验证 (integration-test)
REM   run_intg.bat --panic   构建 panic-test 变体：A-F 后触发 panic，验证故障停机
REM   run_intg.bat --rebuild 强制重新构建 Rust app
REM
REM 前置:
REM   - build_rel\stm32f407_minimal.elf (firmware, 已含 RUSTDIAG)
REM   - D:\projects\mcu\os\joc-app-rust (Rust 工程)
REM   - Renode 在 D:\soft\Renode

setlocal enabledelayedexpansion

set TOOLS_DIR=%~dp0

REM Step 1: 构建 Rust app (integration-test / panic-test feature)
set FEATURES=integration-test
set MODE=intg
if "%~1"=="--panic" (
    set FEATURES=integration-test panic-test
    set MODE=panic
)
echo ============================================================
echo Step 1/3: Building Rust app with %FEATURES% features
echo ============================================================
if not exist "D:\projects\mcu\os\joc-app-rust\build_app.py" (
    echo ERROR: Rust project not found at D:\projects\mcu\os\joc-app-rust
    exit /b 1
)
cd /d D:\projects\mcu\os\joc-app-rust
python build_app.py --features "%FEATURES%"
if %ERRORLEVEL% neq 0 (
    echo ERROR: Rust build failed
    exit /b %ERRORLEVEL%
)

REM Step 2: 运行 Renode + 验证 (--mode=%MODE%)
echo ============================================================
echo Step 2/3: Running Renode simulation + verification (mode=%MODE%)
echo ============================================================
call "%TOOLS_DIR%run_and_verify.bat" stm32f407_jos_intg --mode=%MODE%
set RC=%ERRORLEVEL%

if %RC% equ 0 (
    echo ============================================================
    echo INTEGRATION TEST RESULT: PASS
    echo ============================================================
) else (
    echo ============================================================
    echo INTEGRATION TEST RESULT: FAIL
    echo ============================================================
)

exit /b %RC%