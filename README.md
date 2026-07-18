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
│   ├── main.c                   # 【应用层】只按名取设备：device_manager_get("uart0")
│   ├── selftest.c/.h            # 板载自测类（集成测试）
│   ├── syscalls.c               # newlib 桩，printf 重定向到串口控制台
│   ├── system_stm32f4xx.c       # 官方 CMSIS 系统文件（只配 FPU/VTOR）
│   ├── iface/                   # 【统一驱动接口层】唯一的 device 接口（纯接口，仅 vtable）
│   │   └── device.h/.c          #   统一设备接口：open/close/read/write/ioctl（虚函数）
│   ├── drv/                     # 【驱动层】平台无关！实现 device 接口，持有 HAL 不透明句柄
│   ├── devmgr/                  # 【设备管理层】通用 name->device* 注册表（device_get_binding 风格）
│   │   └── device_manager.h/.c  #   只存/取 device *，不含任何驱动或 HAL 头
│   ├── board/                   # 【板级层】硬件分配以"数据"描述（设备树等价物）
│   │   ├── board.h              #   资源描述 schema（adc/uart/gpio/clock/temp 的资源结构）
│   │   └── stm32f4_discovery.c  #   唯一知道 ADC1/USART1/GPIOD：资源表 + 构造并注册设备
│   │   ├── adc.c/.h             #   通用 ADC，实现 device（read/ioctl: SET_CHANNEL/READ_MV）
│   │   ├── gpio_pin.c/.h        #   通用 GPIO 引脚，实现 device（write/read/ioctl: TOGGLE）
│   │   ├── clock.c/.h           #   通用系统时钟，实现 device（ioctl: GET_SYSCLK_HZ）
│   │   ├── temp_sensor.c/.h     #   通用片内温度传感器，实现 device（依赖 ADC 的 device 接口）
│   │   └── uart.c/.h            #   通用 USART，实现 device（write/read/ioctl: BAUD/BRR/CR1）
│   ├── hal/stm32/               # 【HAL 层】芯片级寄存器操作（唯一碰硬件之处）
│   │   ├── adc_hal.c/.h         #   ADC 寄存器：时钟/采样/序列/单次转换
│   │   ├── gpio_hal.c/.h        #   GPIO 寄存器：配置/置位/复位/翻转/读
│   │   ├── clock_hal.c/.h       #   RCC/FLASH 寄存器：HSE->PLL->168MHz
│   │   ├── uart_hal.c/.h        #   USART 寄存器：波特率/收发
│   │   └── temp_hal.c/.h        #   出厂温度校准字读取（系统存储区）
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

### 分层理念与统一驱动接口

- **统一接口层 `iface/device`**：只定义**一个**纯接口 `device`（参照 `moban/` 的
  `Ibase` 模板——只有虚函数表 vtable，没有 `fun`、没有状态、不 `#include` 任何芯片头）。
  它规定所有驱动必须实现的虚函数：`open / close / read / write / ioctl`。
- **驱动层 `drv/`**：每个外设驱动（adc/gpio/clock/temp/uart）都把
  `device parent;` 作为**结构体首成员**来"继承"该接口，并在 `init()` 里把
  `parent.vtable->read`（等）接到自己的实现上。上层因此只需持有 `device *`。
  **关键点：驱动层完全平台无关**——它只持有 HAL 提供的**不透明句柄**
  （`adc_hal_handle_t *` / `gpio_hal_handle_t *` / `uart_hal_handle_t *`），
  结构体里**不出现** `ADC_TypeDef` / `GPIO_TypeDef` / `USART_TypeDef`，
  编译单元里也**不 `#include` 任何芯片头**。所有寄存器知识都封在 HAL 里。
- **HAL 层 `hal/stm32/`**：唯一直接操作寄存器/系统存储区的地方，也是唯一知道
  `ADC_TypeDef` 等芯片类型的地方。HAL 以**不透明句柄**对外：句柄结构体（含真实的
  外设指针、通道号等）私有定义在 `*_hal.c` 内，驱动只拿到 `typedef struct xxx
  adc_hal_handle_t;` 这种前向声明，永远解引用不到内部成员。
- **板级层 `board/`**：硬件分配**以数据描述**（`g_adc0`/`g_uart0`/`g_led`/... 这组
  `const` 资源结构，相当于一份内联的"设备树"），`board_init()` 遍历这张表、通过
  **按类索引的探测表 `g_probes[driver_type_t]`** 为每个节点构造对应的 HAL 句柄 +
  驱动，并以名字注册进设备管理器。`board_init()` 里**没有 `switch(type)`**——新增一类
  驱动只需在探测表里加一项，不动初始化循环。**它是唯一知道
  `ADC1`/`USART1`/`GPIOD` 以及出厂校准字的地方**——`main.c` 和 `drv/` 都不知道。
- **设备管理层 `devmgr/`**：一个**通用**的 `name -> device *` 注册表
  （`device_manager_register` / `device_manager_get`），不 `#include` 任何驱动或 HAL
  头，只搬运 `device *`。它是主流 RTOS 里 `device_get_binding()` / `rt_device_find()`
  的等价物——应用按名字取设备，完全不接触外设基址或 HAL 句柄。
- **`device` 基类带 `type` + `name`**：每个驱动在 `create()` 时填好自身的
  `device.type`（`DEVICE_TYPE_ADC` 等类标识）和 `device.name`（逻辑名）。这样管理层/
  应用拿到一个 `device *` 就能**按类或按名管理**，而无需知道具体驱动类型——
  对应主流平台里驱动的 `compatible`/类 id 与设备名。
- **应用层 `main.c`**：只做 `board_init()` + `device_manager_get("uart0")`，再经
  `device *` 的 vtable 派发驱动设备；**不 `#include` 任何芯片头，也不出现任何
  `xxx_hal_create`**。换板子时 `main.c` 一行都不用动。

**统一调用形式**：上层应用（`main.c`/`selftest.c`）对**任何**外设都通过同一组虚函数派发访问，
与具体芯片、具体驱动完全解耦。这就是 C 里的 Java 式多态：调用经对象的 vtable 派发
（对应 Java 的 `d.open()`）：

```c
device *d = (device *)adc;          /* 任意驱动都可转成 device * */
d->vtable->open(d);                 /* 初始化（虚函数，经 vtable 派发） */
d->vtable->read(d, &raw, sizeof(raw));  /* 读数据 */
d->vtable->write(d, buf, len);      /* 写数据 */
d->vtable->ioctl(d, ADC_IOCTL_SET_CHANNEL, &ch);  /* 设备相关控制 */
d->vtable->close(d);                /* 关闭 */
```

> 注意：接口 `device` 只提供 `device_init`/`device_deinit`（分配/释放 vtable 并装入
> 默认实现），**不提供** `device_open` 这类自由函数包装；虚函数一律通过
> `obj->vtable->method(obj)` 派发。各驱动的"公开方法"（如 `adc->fun->set_channel(adc, ch)`、
> `uart->fun->getc(uart)`）也只通过各自的 `fun` 表访问，具体实现是 `.c` 里的 `static`，
> 头文件不暴露。

换平台时**只需适配 `hal/<新平台>/` 并新增一份 `board/<新板>.c`（板级资源数据 + 构造）**，
**驱动层 `drv/`、设备管理层 `devmgr/`、应用层 `main.c`、接口层 `device` 全部一行都不用动**。
因为驱动只依赖 HAL 的不透明句柄，新 HAL 只要提供相同签名的 `*_hal_create` / `*_hal_*` 函数，
驱动源码即可原样复用；而 `main.c` 只按名字取设备，连板级差异都不感知。

> 这套分层对应主流嵌入式平台的两大解耦机制：
> - **设备树（Device Tree）** → 本工程的 `board/<板>.c` 资源表（硬件分配数据化）；
> - **设备/驱动管理** → 本工程的 `devmgr/` 注册表（按名取设备 `device_manager_get`）。
> 两者互补：设备树提供"资源在哪（数据）"，管理器提供"应用怎么拿到（按名）"。

> 示例：`temp_sensor` 只持有 **`device *`**（ADC 的统一接口）而非具体的
> `adc`，读温度时通过 `adc->vtable->ioctl(adc, ...)` / `adc->vtable->read(adc, ...)`
> 临时切到 CH16、读值、再切回，因此只要新平台提供一个实现了 `device` 的 ADC 驱动，
> 温度传感器即可直接复用。温度传感器的芯片专属出厂校准字由**板级层**（`board/stm32f4_discovery.c`
> 通过 `temp_hal_ts_cal1/2` 读取）在构造时传入，所以 `temp_sensor` 驱动本身不碰任何寄存器。

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

- 每个类包含：`*Fun`（公开方法表：create/destroy/init/deinit/...，以及
  `set_channel`/`getc` 等类型化方法）与 `deviceVtable`（统一虚函数表：
  open/close/read/write/ioctl，可被驱动重写）。
- 统一接口继承：每个驱动在结构体中把 `device parent;` 作为**首成员**来"继承"
  统一接口 `device`（接口只持有 `struct deviceVtable *vtable;` 这个虚函数表指针，
  没有任何 `fun`、没有状态，不碰芯片头），并在 `init()` 里把
  `parent.vtable->read`（等）接到自己的 `static` 实现；
  因为 `device` 是首成员，`(device*)driver` 与 `(driver*)device` 可安全互转。
- 调用方式（参照 `moban/Ibase` 与 Java 的多态思路）：
  - 公开方法：`self->fun->method(self)` —— 具体实现是 `.c` 里的 `static`，头文件不暴露；
  - 虚方法：`self->vtable->method(self)`（基类持有者）或
    `son->parent.vtable->method((device*)son)`（子类内部调自己重写的虚函数），
    全部经对象的 vtable 派发，没有任何自由函数包装。

当前类关系（均遵循"统一接口层 → 驱动层 → HAL 层"三层）：

- **统一接口 `device`**（在 `iface/device.h`，参照 `moban/` 的 `Ibase` 纯接口模板）：
  定义虚函数 `open/close/read/write/ioctl`，是所有驱动的基类/接口类；
- **五个驱动全部"继承"并实现 `device`**（把 `device parent;` 作为首成员，
  在 `init()` 里填充 `parent.vtable`）：
  `adc`、`gpio_pin`、`clock`、`temp_sensor`、`uart`（均为平台无关名，
  结构体里只持有 HAL 不透明句柄，不出现任何芯片类型）；
- `temp_sensor` 内部只持有 **`device *`**（ADC 的统一接口，而非具体的
  `adc`），读温度时通过 `adc->vtable->ioctl(adc, ...)` / `adc->vtable->read(adc, ...)`
  临时切到 CH16、读值、再切回，因此与 ADC 具体实现解耦；
- 各驱动把寄存器操作全部委托给 `hal/stm32/` 下对应的 `*_hal`，且只通过
  **不透明句柄**访问——驱动源码里看不到 `ADC_TypeDef` / `GPIO_TypeDef` / `USART_TypeDef`。
- **调试连不上**：确认 ST-Link 已插入、板子供电正常；GDB Server 默认端口 61234。

## 板载自测（BIST）与 PC 陪测

固件上电后先跑 **板载自测**（`selftest` 类，遵循 moban 模板），逐项检查：

- `clock`：PLL 已锁定且被选为系统时钟，`sysclk == 168 MHz`；
- `uart`：USART1 已使能 TX+RX 且 `BRR` 波特率正确（不实际发数，避免污染串口）；
- `gpio`：翻转 PD12 后读回电平确实变化，再翻回原值。
- `adc`：切换 ADC 到片内 **VREFINT（CH17，约 1.21 V）** 读一次，
  12 位原始值应在合理区间（实测约 1517，对应 VDDA≈3.3 V），借此验证
  ADC 时钟 / 序列 / EOC / 数据读取整条通路；读完后切回外部通道（PA0）。
- `temp`：用 `temp_sensor` 读片内温度传感器（ADC1_IN16），按出厂校准
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
