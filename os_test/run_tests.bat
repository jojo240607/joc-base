@echo off
rem ===========================================================================
rem PC host unit-test runner (docs/ostest.md section 8). Build and run all
rem tests under os_test/common with the native gcc: atomic / lock / barrier /
rem name_table / pool. Pure host logic; HW semantics covered by BIST / HIL.
rem
rem Usage:  os_test\run_tests.bat
rem Deps:   gcc (MinGW-w64 / any x86 gcc)
rem ===========================================================================
setlocal enabledelayedexpansion
set "CC=gcc"
set "CFLAGS=-O2 -Wall -Wextra -std=gnu11 -I../src"
rem pool_test links the OOC memory pool implementation (host build).
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
