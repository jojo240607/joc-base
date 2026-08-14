@echo off
REM run_and_verify.bat — 一键运行 Renode resc + verify_app.py 断言验证
REM
REM 用法:
REM   run_and_verify.bat                   默认 resc (stm32f407_jos_app.resc)
REM   run_and_verify.bat <resc_name>       指定 resc 文件名 (在 tools\renode\ 下)
REM   run_and_verify.bat <resc_name> --min-ticks=500
REM
REM 输出:
REM   日志文件保存在 build_rel\renode_<resc_name>_<YYYYMMDD_HHMMSS>.log
REM   PASS/FAIL 结果打印到控制台

setlocal enabledelayedexpansion

set TOOLS_DIR=%~dp0
set PROJECT_DIR=%TOOLS_DIR%..\..
set BUILD_DIR=%PROJECT_DIR%\build_rel
set RENODE_DIR=D:\soft\Renode

REM 默认 resc
set RESC_NAME=stm32f407_jos_app
if not "%~1"=="" set RESC_NAME=%~1
set RESC_FILE=%TOOLS_DIR%%RESC_NAME%.resc
if not exist "%RESC_FILE%" (
    echo ERROR: resc file not found: %RESC_FILE%
    exit /b 1
)

REM 时间戳
for /f "tokens=2 delims==" %%I in ('wmic os get localdatetime /value') do set DT=%%I
set TIMESTAMP=%DT:~0,4%%DT:~4,2%%DT:~6,2%_%DT:~8,2%%DT:~10,2%%DT:~12,2%
set LOG_FILE=%BUILD_DIR%\renode_%RESC_NAME%_%TIMESTAMP%.log

REM 提取额外参数 (--min-ticks, --max-ticks, --mode 等)
set EXTRA_ARGS=
:parse_args
if not "%~1"=="" (
    if "%~1"=="--min-ticks" (
        set EXTRA_ARGS=%EXTRA_ARGS% --min-ticks=%~2
        shift
        shift
        goto parse_args
    )
    if "%~1"=="--max-ticks" (
        set EXTRA_ARGS=%EXTRA_ARGS% --max-ticks=%~2
        shift
        shift
        goto parse_args
    )
    if "%~1"=="--min-ticks=" (
        set EXTRA_ARGS=%EXTRA_ARGS% %~1
        shift
        goto parse_args
    )
    if "%~1"=="--max-ticks=" (
        set EXTRA_ARGS=%EXTRA_ARGS% %~1
        shift
        goto parse_args
    )
    REM --mode <value> (cmd splits '--mode=xxx' on '=' when calling a .bat,
    REM so the two-token form is what actually arrives; rebuild the single
    REM token here so python sees --mode=xxx whole)
    if "%~1"=="--mode" (
        set EXTRA_ARGS=%EXTRA_ARGS% --mode=%~2
        shift
        shift
        goto parse_args
    )
    REM passthrough: --mode=xxx (direct cmd-line call, single token)
    set _FIRST_CHAR=%~1
    if not "!_FIRST_CHAR:~0,6!"=="--mode" goto skip_mode
    set EXTRA_ARGS=%EXTRA_ARGS% %~1
    shift
    goto parse_args
    :skip_mode
    shift
    goto parse_args
)

echo ============================================================
echo Renode run + verify
echo   resc:  %RESC_FILE%
echo   log:   %LOG_FILE%
echo ============================================================

REM 运行 Renode
echo [run] Starting Renode...
"%RENODE_DIR%\renode.exe" --disable-xwt --console -e "include @%RESC_FILE%" > "%LOG_FILE%" 2>&1
set RENODE_RC=%ERRORLEVEL%
echo [run] Renode exit code: %RENODE_RC%

REM 运行 verify_app.py (cd 到本目录，避免跨盘路径解析问题)
cd /d "%TOOLS_DIR%"
echo [verify] Running verify_app.py...
python "verify_app.py" "%LOG_FILE%" %EXTRA_ARGS%
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