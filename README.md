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
tick 0
tick 1
tick 2
...
```

同时板载绿色 LED（LD4，PD12）每 0.5 秒翻转一次。

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
`clock`、`gpio_pin` 为独立叶子类。
- **调试连不上**：确认 ST-Link 已插入、板子供电正常；GDB Server 默认端口 61234。

## 烧录（ST-Link）

`tools\flash.bat` 会先尝试 STM32CubeProgrammer（connect-under-reset, 1 MHz），
失败则自动回退到 OpenOCD（`board/stm32f4discovery.cfg`）。

> 已实测：OpenOCD 能读到 `SWD DPIDR 0x2ba01477`（SWD 物理链路正常），
> 但报 `Examination failed` / `reset will not halt`。这说明 **SWCLK/SWDIO/GND
> 接线正确，但 NRST（复位线）没接到 ST-Link**——独立 ST-LINK 调试器尤其容易只接了
> 三线而漏掉复位。把 ST-LINK 的 **NRST 接到 MCU 的复位脚（NRST）** 后重新烧录即可。
> 若仍不行，给板子断电再上电（冷复位）后再试。
