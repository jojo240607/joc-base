@echo off
rem ===========================================================================
rem PC 主机单元测试运行器（docs/ostest.md §8）：用本机 gcc 编译并运行 os_test/common
rem 下的全部单测（atomic / lock / barrier / name_table / pool），汇总 PASS/FAIL
rem 与退出码。固件无关部分在主机验证；硬件相关语义由板载 BIST / HIL 覆盖。
rem
rem 用法：  os_test\run_tests.bat
rem 依赖：  gcc（MinGW-w64 / 任意 x86 gcc）
rem ===========================================================================
setlocal enabledelayedexpansion
set "CC=gcc"
set "CFLAGS=-O2 -Wall -Wextra -std=gnu11 -I../src"
rem pool_test 需要链接的内存池实现（host 端直接编译）
set "LIBS=..\src\common\pool.c"
set FAILS=0
set PASSED=0

echo === jOS host unit-tests ===
for %%T in (atomic_test lock_test barrier_test name_table_test pool_test) do (
    echo --- building %%T ---
    %CC% %CFLAGS% common\%%T.c %LIBS% -o %%T.exe
    if errorlevel 1 (
        echo   %%T: BUILD FAIL
        set /a FAILS+=1
    ) else (
        %%T.exe > nul
        if errorlevel 1 (
            echo   %%T: FAIL
            set /a FAILS+=1
        ) else (
            echo   %%T: PASS
            set /a PASSED+=1
        )
    )
)

echo === summary: passed=%PASSED% failed=%FAILS% ===
if %FAILS%==0 ( exit /b 0 ) else ( exit /b 1 )
