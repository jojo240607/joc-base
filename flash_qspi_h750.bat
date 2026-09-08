#
# flash_qspi_h750.bat — deploy the Rust app to the STM32H750B-DK external
# QSPI flash (W25Q128 @ 0x90000000) via OpenOCD.
#
# This is the "QSPI 烧写接口" host-side half: OpenOCD drives the QUADSPI
# controller directly (SWD + core), erases/programs the external flash, so
# the on-target QSPI command (console `QSPI`) is only needed for on-board
# verification / OTA, not for the initial deployment.
#
# Prereqs:
#   1. OpenOCD >= 0.11 built with stm32h7x_qspi flash driver
#      (e.g. https://github.com/openocd-org/openocd, or xpack build).
#      Adjust OPENOCD below to your install.
#   2. app.bin built by joc-app-rust (python build_app.py), copied here.
#   3. Board powered via ST-LINK (CN1) — the ST-LINK on the DK drives SWD.
#
# Usage: flash_qspi_h750.bat [app.bin]
#
# NOTE: the internal flash (RTOS system) is flashed separately, see
# flash_sys_h750.bat. This script only touches the external QSPI flash.
#
setlocal
set OPENOCD=openocd
set APP=%1
if "%APP%"=="" set APP=app.bin
if not exist "%APP%" (
  echo [ERR] %APP% missing. Build it in joc-app-rust first: python build_app.py
  exit /b 1
)
%OPENOCD% -f interface/stlink.cfg -f tools/openocd/h750_qspi.cfg ^
  -c "program %APP% 0x90000000 verify reset exit"
endlocal
