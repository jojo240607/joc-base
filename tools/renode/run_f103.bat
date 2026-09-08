@echo off
REM run_f103.bat — 一键运行 F103 固件 in Renode + verify_app.py 验证
REM
REM 用法:
REM   run_f103.bat            默认构建 + 运行 + 验证
REM   run_f103.bat --no-build 跳过构建，直接运行 Renode
REM
REM 输出:
REM   日志保存在 build_f103\renode_f103_<YYYYMMDD_HHMMSS>.log
REM   PASS/FAIL 结果打印到控制台

setlocal enabledelayedexpansion

set TOOLS_DIR=%~dp0
set PROJECT_DIR=%TOOLS_DIR%..\..
set BUILD_DIR=%PROJECT_DIR%\build_f103
set RENODE_DIR=D:\soft\Renode
set RESC_FILE=%TOOLS_DIR%stm32f1_jos.resc

REM Step 0: 可选构建
if not "%~1"=="--no-build" (
    echo [run] Building F103 firmware...
    cd /d "%BUILD_DIR%"
    cmake --build .
    if !ERRORLEVEL! neq 0 (
        echo ERROR: Build failed
        exit /b 1
    )
)

REM 时间戳
for /f "tokens=2 delims==" %%I in ('wmic os get localdatetime /value') do set DT=%%I
set TIMESTAMP=%DT:~0,4%%DT:~4,2%%DT:~6,2%_%DT:~8,2%%DT:~10,2%%DT:~12,2%
set LOG_FILE=%BUILD_DIR%\renode_f103_%TIMESTAMP%.log

echo ============================================================
echo Renode run + verify (F103 minimal)
echo   resc:  %RESC_FILE%
echo   log:   %LOG_FILE%
echo ============================================================

REM 运行 Renode
echo [run] Starting Renode...
"%RENODE_DIR%\renode.exe" --disable-xwt --console -e "include @%RESC_FILE%" > "%LOG_FILE%" 2>&1
set RENODE_RC=%ERRORLEVEL%
echo [run] Renode exit code: %RENODE_RC%

REM 运行 verify_app.py (minimal mode: pong + zero_faults + ticks)
cd /d "%TOOLS_DIR%"
echo [verify] Running verify_app.py (mode=minimal, min-ticks=100)...
python "%TOOLS_DIR%verify_app.py" "%LOG_FILE%" --mode=minimal --min-ticks=100
set VERIFY_RC=%ERRORLEVEL%

if %VERIFY_RC% equ 0 (
    echo ============================================================
    echo RESULT: PASS
    echo ============================================================
) else (
    echo ============================================================
    echo RESULT: FAIL
    echo ============================================================
)

exit /b %VERIFY_RC%