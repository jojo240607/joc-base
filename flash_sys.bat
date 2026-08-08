@echo off
REM 只烧系统区（RTOS + 驱动 + C 固件）。不会触碰 APP_FLASH 块(sector 7~)中的应用分区。
REM 用法：先整体构建 joc-base，再 flash_sys.bat
"D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\bin\openocd.exe" -s "D:\soft\openocd\openocd-4e78563-i686-w64-mingw32\share\openocd\scripts" -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program build/stm32f407_minimal.bin verify reset exit 0x08000000"
