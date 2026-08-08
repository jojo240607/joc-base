@echo off
REM 只烧应用分区（阶段 2：独立 App 镜像到 APP_FLASH 块，sector 7 起）。
REM 用法：在 joc-app-rust 跑 build_app.py 产出 app.bin，拷贝到本目录后 flash_app.bat。
REM 不会触碰系统区(sector 0~6)。APP 镜像须 < 384K。
if not exist app.bin (
  echo [ERR] 找不到 app.bin。先到 joc-app-rust 跑: python build_app.py
  exit /b 1
)
"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe" -s "D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts" -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program app.bin verify reset exit 0x08060000"
