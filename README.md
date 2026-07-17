# STM32F407 最小工程（STM32F4 Discovery）

基于 **STM32F4 Discovery（STM32F407VGT6）** 的最小可调试工程：

- 构建：CMake + Ninja（arm-none-eabi-gcc，来自 STM32CubeCLT）
- 下载/调试：板载 ST-Link（SWD），使用 `ST-LINK_gdbserver`
- 串口：USART1（PA9=TX, PA10=RX）@ 115200 8N1 → 外接 USB-TTL → PC **COM8**
- 时钟：HSE 8 MHz → PLL → 168 MHz
- 代码风格：**面向对象 C（OOC）**，外设驱动均建模为类（见 `moban/` 模板）

> 说明：STM32F4 Discovery 板载 ST-LINK/V2 **不带**虚拟串口（VCP），
> 所以串口需外接一个 USB-TTL（CH340/CP2102 等）接到 PA9/PA10，
> 在电脑端会枚举成 COM8（具体端口号以设备管理器为准）。

## 目录结构

```
.
├── CMakeLists.txt
├── cmake/toolchain.cmake        # arm-none-eabi 工具链
├── src/
│   ├── main.c                   # 用 OOC 类组装：clock + uart_stm32 + gpio_pin
│   ├── clock.c/.h               # OOC 系统时钟类（HSE->PLL 168MHz）
│   ├── serial.c/.h              # OOC 抽象串口基类（putc/puts 虚函数）
│   ├── uart_stm32.c/.h          # OOC 具体串口类，继承 serial，USART1 PA9/PA10
│   ├── gpio_pin.c/.h            # OOC GPIO 引脚类（LED 等）
│   ├── adc_stm32.c/.h           # OOC ADC 类（ADC1/2/3，单次转换 + mV 换算）
│   ├── temp_sensor_stm32.c/.h   # OOC 片内温度传感器类（ADC1_IN16 + 出厂校准换算℃）
│   ├── syscalls.c               # newlib 桩，printf 重定向到串口控制台
│   ├── system_stm32f4xx.c       # 官方 CMSIS 系统文件（只配 FPU/VTOR）
│   ├── device/                  # 官方 STM32F4 设备头（仅 F407）
│   └── cmsis/                   # 官方 CMSIS-Core 头
├── moban/                       # OOC 代码模板（base/son/ison 等）
├── startup/startup_stm32f407xx.s
├── linker/STM32F407VGTX_FLASH.ld
├── tools/
│   ├── flash.bat                # 烧录
│   ├── debug.bat                # 一键调试（启动 GDB Server + gdb）
│   ├── start_gdbserver.bat      # 仅启动 GDB Server
│   └── gdbinit.txt
└── .vscode/                     # VS Code 构建/调试配置
```

## 硬件连接

| STM32F407 | USB-TTL | 说明 |
|-----------|---------|------|
| PA9 (USART1_TX) | RX | 开发板发 |
| PA10 (USART1_RX) | TX | 开发板收 |
| GND | GND | 共地 |

ST-Link 用板载的 SWD 接口（USB 直连电脑即可），无需额外接线。

## 构建

```bat
cmake -S . -B build -G Ninja
cmake --build build -j4
```

产物：`build/stm32f407_minimal.elf`、`.bin`、`.hex`。

## 烧录（ST-Link）

```bat
tools\flash.bat
```

或手动：

```bat
"D:\soft\ST\STM32CubeCLT_1.19.0\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe" ^
    -c port=SWD -w build\stm32f407_minimal.bin 0x08000000 -rst
```

## 调试（ST-Link + GDB）

方式一（命令行，推荐验证用）：

```bat
tools\debug.bat
```

它会后台启动 `ST-LINK_gdbserver`（端口 61234），再用 `arm-none-eabi-gdb`
连接、`load` 烧录、`monitor reset halt`、在 `main` 下断点并运行。

方式二（VS Code）：安装 **C/C++** 扩展，按 `F5` 选择 `Debug ST-Link (F407)`。
（launch.json 已配置 `preLaunchTask` 自动启动 GDB Server 并烧录。）

## 查看串口输出（COM8）

烧录后，用任意串口工具以 **115200 8N1** 打开 **COM8**，应看到：

```
Hello from STM32F407 Discovery (OOC)!
System clock: 168000000 Hz, USART1 @ 115200 8N1

--- On-board self-test (BIST) ---
[BIST] clock : PASS
[BIST] uart  : PASS
[BIST] gpio  : PASS
       VREFINT raw=1517 (expect ~1500)
[BIST] adc   : PASS
       die temp = 47.5 C (cal1=941 cal2=1197)
[BIST] temp  : PASS
SELF-TEST: PASS
READY. Commands: PING / ECHO <text> / BIST / ADC [ch] / TEMP
```

随后固件进入命令行循环，可手动输入 `PING`（回 `PONG`）、`ECHO <文本>`（原样回显）、
`BIST`（重跑自测并打印 `SELF-TEST:`）、`ADC [ch]`（采样 ADC 并打印 `raw`/`mV`，
缺省通道 0 = PA0；例如 `ADC 16` 读片内温度传感器）、`TEMP`（读芯片自身温度，
打印 `raw`、出厂校准 `cal1/cal2` 与换算后的 `C=xx.x`），每收到一条命令绿色 LED（LD4，PD12）翻转一次。

> 若 COM8 不是你的端口，以 Windows 设备管理器里显示的为准。

## 常见问题

- **编译报找不到 arm-none-eabi-gcc**：把 STM32CubeCLT 的
  `GNU-tools-for-STM32\bin` 加入 PATH，或用 VS Code（已写死路径）。
- **串口无输出**：确认 USB-TTL 的 TX/RX 与 PA10/PA9 交叉连接、共地，
  且波特率为 115200。

## OOC 代码组织（参考 moban/）

外设驱动全部按 `moban/` 的面向对象 C 模板编写：

- 每个类包含：`*Fun`（静态方法表：create/destroy/init/deinit/...）与
  `*Vtable`（虚函数表：可被派生类重写）。
- 继承：派生类在结构体中嵌入父类作为首成员，vtable 的首成员是父类的 vtable
  （如 `uart_stm32Vtable.parent` 是 `serialVtable`），并用
  `union { parent; vtable*; }` 保证布局兼容。
- 调用方式：静态方法 `self->fun->method(self)`，虚方法 `self->vtable->method(self)`。

当前类关系：`uart_stm32` ─继承─> `serial`（抽象串口基类）；
`clock`、`gpio_pin`、`adc_stm32`、`temp_sensor_stm32` 为独立叶子类
（`temp_sensor_stm32` 内部复用 `adc_stm32` 对象，读温度时临时切到 CH16）。
- **调试连不上**：确认 ST-Link 已插入、板子供电正常；GDB Server 默认端口 61234。

## 板载自测（BIST）与 PC 陪测

固件上电后先跑 **板载自测**（`selftest` 类，遵循 moban 模板），逐项检查：

- `clock`：PLL 已锁定且被选为系统时钟，`sysclk == 168 MHz`；
- `uart`：USART1 已使能 TX+RX 且 `BRR` 波特率正确（不实际发数，避免污染串口）；
- `gpio`：翻转 PD12 后读回电平确实变化，再翻回原值。
- `adc`：切换 ADC 到片内 **VREFINT（CH17，约 1.21 V）** 读一次，
  12 位原始值应在合理区间（实测约 1517，对应 VDDA≈3.3 V），借此验证
  ADC 时钟 / 序列 / EOC / 数据读取整条通路；读完后切回外部通道（PA0）。
- `temp`：用 `temp_sensor_stm32` 读片内温度传感器（ADC1_IN16），按出厂校准
  `TS_CAL1`(30 ℃)/`TS_CAL2`(110 ℃) 线性插值换算摄氏温度，结果应在合理区间
  （实测约 47 ℃，即 168 MHz 全速运行的裸die自升温），证明温度传感器驱动正确。

全过打印 `SELF-TEST: PASS`，否则 `FAIL`。随后进入命令行循环，供 **PC 陪测脚本**
验证 USART 的 TX+RX 回环：

```bat
pip install pyserial
python tools\companion_test.py COM8 115200
```

**一键验证**（编译 → 烧录 → 陪测）：

```bat
tools\test.bat [PORT] [BAUD]      # 默认 COM8 115200
```

`test.bat` 会先 `cmake --build build`，再 `call tools\flash.bat`（OpenOCD connect-under-reset），
最后跑陪测脚本；任一步失败即中止并打印对应错误。看到 `=== ALL GREEN ===` 即全过。

脚本会：连上即发 `BIST` → 读 `SELF-TEST:` 行 → 发 `PING` 期望 `PONG`
→ 发 `ECHO hello-companion` 期望回显，最后打印 `OVERALL: PASS/FAIL` 并以退出码 0/1 表示结果。
（板端对命令做了本地回显，脚本会自动忽略回显行，只匹配板子的应答。
之所以"连上即发 BIST"而不是等开机横幅，是因为板子在烧录后立刻启动、开机输出早已发完，
PC 端串口缓冲不会保留连上之前的字节。）

## 烧录（ST-Link）

`tools\flash.bat` 用 **OpenOCD `program`** 命令（`interface/stlink.cfg` + `target/stm32f4x.cfg`，
`reset_config srst_only connect_assert_srst` 即 connect-under-reset），失败则回退 STM32CubeProgrammer。

> 已实测（NRST 已接）：`Examination succeed` → `Programming Finished` → `Verified OK`
> → `Resetting Target`，一次成功。`connect_assert_srst` 依赖 ST-Link 的 **NRST 接到 MCU 复位脚**；
> 若没接 NRST，去掉该选项、改用手动按板载 RESET 键后再烧录也可。
