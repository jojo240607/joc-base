@echo off
rem ============================================================
rem  One-shot: build F407 release firmware + Renode dual-partition
rem  run + Rust integration-test (intg) verification.
rem  Equivalent to:  python build_rel_intg.py %*
rem  Usage:
rem    build_rel_intg.bat             full (build + run + verify)
rem    build_rel_intg.bat --no-build  reuse existing artifacts
rem ============================================================
cd /d %~dp0
python build_rel_intg.py %*
if errorlevel 1 (
  echo.
  echo [FAIL] verification failed, see output above
  exit /b 1
)
echo [OK] build + Renode verification passed
