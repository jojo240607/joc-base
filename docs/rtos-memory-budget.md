# joc-base RTOS 运行时内存占用评估

> 数据来源：本仓库 `build_rel/`（轨B，发布构建 `RTOS_SELFTEST=OFF`）的
> `stm32f407_minimal.elf` 链接映射（`.map`）+ `arm-none-eabi-size` + `arm-none-eabi-nm`。
> 芯片：STM32F407VGT6，总内存 **主 SRAM 128KB（@0x20000000）+ CCM 64KB（@0x10000000）= 192KB**，
> 外加 Flash 1MB。
>
> 本文档为"生产运行"真实占用。开发版（`RTOS_SELFTEST=ON`）差异见 §6。

## 1. 总览

| 存储域 | 总容量 | RTOS/系统占用 | 利用率 | 备注 |
|---|---|---|---|---|
| Flash | 1024 KB | ~91.4 KB（text 90976 + data 400） | 8.9% | 余量充裕 |
| 主 SRAM | 128 KB | ~27.7 KB（.bss 0x6c08） + 内核 .data 392B | 21.7% | 不含 APP_RAM 区；DMA 缓冲在此 |
| CCM | 64 KB（用 63KB，顶 1KB 给 MSP） | 19.3 KB（.ccm_bss 0x4c00） | 30.5% | 仅 CPU，TCB/任务栈在此 |
| APP_RAM（给 Rust App） | 91 KB（0x20007000–0x2001DC00） | 0（归 App） | — | 见 §7 |

> 注：`arm-none-eabi-size` 的 `bss` 列 = 主 SRAM `.bss` + CCM `.ccm_bss`，合计约 47.8 KB。
> 拆分后**主 SRAM 自身 `.bss` = 0x20006e08 − 0x20000200 = 0x6c08 = 27,656 B**。

## 2. Flash 占用

`arm-none-eabi-size` 实测：`text = 90976 B`，`data = 400 B`（初始化值，随 text 载入）。

| 项 | 大小 | 说明 |
|---|---|---|
| `.text`（代码 + 向量表） | 90,976 B | RTOS 内核 + 驱动 + HAL + BIST + app 桩 |
| `.data`（初始值） | 400 B | 从 Flash 载入主 SRAM `.data` |
| **Flash 合计** | **~91.4 KB / 1024 KB** | 占 8.9% |

## 3. 主 SRAM 占用（@0x20000000，DMA 可达）

| 区域 | 地址范围 | 大小 | 内容 |
|---|---|---|---|
| `.data` | 0x20000000–0x20000188 | 392 B | 已初始化全局（含 `g_app_ctx`） |
| `.bss` | 0x20000200–0x20006e08 | 27,656 B | 未初始化全局/静态（见 §4 拆解） |
| `_Min_Heap_Size` | 0x20006e08–0x20007008 | 512 B | 静态堆下限（实际堆可延伸到 APP_RAM 前） |
| 空隙 | 0x20007008–0x20007000… | — | 系统 .bss 收缩后留出的缓冲带（见 §7） |
| `.app_slot` | 0x2001dc00–0x2001dcbc | 188 B | 应用分区契约 `g_app_slot` |
| APP_RAM（Rust App） | 0x20007000 起 | 91 KB | **归 App，非 RTOS** |

### 3.1 主 SRAM `.bss` 拆解（27,656 B）

最大两块：

| 变量 / 来源 | 大小 | 说明 |
|---|---|---|
| `g_sys_heap[0x2000]` | 8,192 B (8 KB) | **静态系统堆**（SYS_STATIC_HEAP）。发布版放主 SRAM `.bss`，开发版放 CCM。原为 32KB，实测 57 板级设备峰值 malloc 仅 6124B，故下调到 8KB 留余量 |
| 其余内核/驱动/库 bss | ~19.5 KB | 见下 |

内核/驱动/库 bss（~19.5 KB）主要构成：

| 来源 | 大小 | 内容 |
|---|---|---|
| `device_manager.c` | 5,648 B | `g_hnode_store[60]` + `g_node_store[60]` 静态节点池（DEVICE_MANAGER_MAX=40 + headroom，避免 malloc） |
| `usb_hal.c` `g_pdev` | 1,516 B | ST USB OTG 核心句柄 `USB_OTG_CORE_HANDLE` |
| `irq.c` `g_irq` | 3,136 B | IRQ 管理器注册表（每 IRQ slot 的回调/ctx） |
| `irq_manager.c` `g_mgr` | 1,152 B | 优先级/类别管理表 |
| `rtos_watchdog.c` `g_marathon_stack_arr` | 3,072 B | marathon 看门狗栈快照数组 |
| `rtos_sched_analysis.c` | 624 B | WCRT/P/T/C 调度分析表 |
| `kobj.c` `g_kobj` | 1,536 B | 内核对象注册表 |
| `bh.c` `g_bh` | 320 B | 底半部/BH 任务表 |
| `sched.c` / `mpu.c` 各 `g_*` | ~1,120 B | 就绪位图/链表/诊断快照 |
| libc_nano | ~324 B | `__sf`（stdio FILE 表）、`errno`、`__malloc_*` 锁 |
| USB/CDC/console/log/app 小量 | ~768 B | `usb_str_buf`、`cdc_rx_buf`、`g_log`、`g_console` 等 |
| 对齐填充 | ~304 B | 链接器 4/8 字节对齐空隙 |

> **结论**：`.bss` 总量里 8KB 是堆（功能性预留，非内核开销），真正 RTOS 内核+驱动+库
> 静态占用约 **19.5 KB**。RTOS 是"零堆启动"设计——TCB、任务栈全静态入 CCM，
> 不依赖链接器堆 `_end..__HeapLimit`。

## 4. CCM 占用（@0x10000000，仅 CPU）

实测 `.ccm_bss = 0x4c00 = 19,264 B`（区域上限 63KB，`_estack=0x10010000` 顶 1KB 给 MSP）。

| 块 | 来源 | 大小 | 说明 |
|---|---|---|---|
| `g_main_stack` | task_app_main.c | 8,192 B | main 任务栈（含 app_host 拉起、设备 open） |
| `g_idle_stack` | task_idle.c | 512 B | idle 任务（prio 31，仅自旋） |
| `app_host_stack` | app_slot_boot.c | 2,048 B | 异步拉起 Rust App 任务 |
| `g_task_pool`（TCB） | task.c | 4,608 B | **48 槽 TCB 池**（RTOS_MAX_TASKS=48 × ~96B/TCB） |
| `g_timer_stack` | timer.c | 1,024 B | 定时器回调任务 |
| `g_wq_stack` | bh.c | 1,024 B | 工作队列 worker |
| **CCM 小计** | | **19,264 B** | 占 CCM 63KB 的 30.5% |
| MSP 主栈 | 顶部 1KB | 1,024 B | 中断/异常栈（独立，不算 ccm_bss） |

> CCM 只放"纯 CPU、永不 DMA"的对象（任务栈 + TCB 池）；UART/USB/SD 等 DMA 缓冲
> 必须留在主 SRAM。CCM 不在默认 MPU 区，非特权任务访问需 Region 5 开放（见 rtos-design.md）。

## 5. 运行时动态内存（堆，malloc）

RTOS 内核对象全静态，不 malloc。堆消费者：

| 动态消费者 | 位置 | 量级 |
|---|---|---|
| 板级设备驱动结构体（`board_init` 给 ~57 设备逐一 malloc） | 主 SRAM | 实测峰值 **6,124 B** |
| UART/ADC/DAC 等 per-engine 状态（open() 时 malloc） | 主 SRAM | 每设备数十~数百 B |
| libc malloc 自由链表 | 主 SRAM | ~16 B |
| **可用堆上限** | 主 SRAM | `g_sys_heap`（8KB）+ 链接器堆 `_end..__HeapLimit`（512B 下限） |

> 历史坑（已根治）：开发版主 SRAM `.bss` 曾占满 124KB，链接堆仅剩 ~440B，
> 57 设备 malloc 第 54 个（usb0）耗尽堆 → usb0 未注册。现用 `SYS_STATIC_HEAP`
> 把 `g_sys_heap` 作静态后备堆，发布版放主 SRAM（8KB），开发版放 CCM（7KB，物理上限）。

## 6. 两构建差异（重要）

| 项 | 发布版（build_rel，实测） | 开发版（RTOS_SELFTEST=ON） |
|---|---|---|
| CCM `.ccm_bss` | 19.25 KB | ~56 KB（自测任务栈全进 CCM） |
| 自测任务栈 | 无 | 数十个 512B~1024B 栈 + p4 36 filler |
| 主 SRAM `.bss` | 27.7 KB | ~124 KB（自测代码/数据） |
| Flash | 91 KB | ~134 KB |
| 静态堆 | 充裕（8KB 主 SRAM） | 几乎为 0（需 CCM heap 兜底 7KB） |
| USB0 注册风险 | 无 | 高（堆耗尽），靠 SYS_STATIC_HEAP 解决 |

> 评估系统预算时**不能直接用发布版数字当开发版**——开发版 CCM 会飙到 ~89%，
> 此时 57 设备 malloc 必须靠 `SYS_STATIC_HEAP`（CCM 专用段）兜底。

## 7. 近期内存优化记录

### 7.1 g_sys_heap 32KB → 8KB（发布版）
- 原 `SYS_HEAP_SIZE=0x8000u`（32KB）过大。实测峰测 57 设备累计 malloc 需求 6124B。
- 改 `CMakeLists.txt` 发布版 `SYS_HEAP_SIZE=0x2000u`（8KB，留 ~2KB 余量）；
  `src/syscalls.c` 默认兜底同步改为 `0x2000u`。
- 效果：主 SRAM `.bss` 由 52,232 B 降至 27,656 B（**−24 KB**）。开发版保持 `0x1C00u`（7KB）不动。

### 7.2 APP_RAM 起点下移，App 区 67KB → 91KB（方案 B）
- 系统 `.bss` 收缩 24KB 后，0x20006e08 到原 APP_RAM 起点 0x2000D000 间空出 ~24KB 自由空间。
- 把 `APP_RAM_ORIGIN` 由 `0x2000D000` 下移到 **`0x20007000`**（留 ~0x1F8 余量防系统 .bss 反弹），
  `APP_RAM_LENGTH` 由 `0x10C00` 增至 **`0x16C00`（91KB）**。
- 同步修改 `joc-app-rust/app.ld` 的 `APP_RAM` ORIGIN/LENGTH，与 joc-base 注入值严格一致。
- **安全核验**：`g_irq[]=0x20002374`、`g_app_loaded=0x2000343c` 均低于 `0x20007000`，
  加载器清零 APP_RAM 不会破坏系统 `.bss`（无 IRQ 风暴）。
- 重测启动通过：usb0 注册 OK、RUST app mounted、sensor 循环 950+、ctrl 心跳 seq 3500+，无 fault。

## 8. 结论

- **生产固件（当前 build_rel）** RTOS 静态占用 ≈ Flash 91KB + 主 SRAM 27.7KB + CCM 17.4KB（不含 App）。
  CCM 用 30.5%、主 SRAM 用 21.7%（不含 APP_RAM）、Flash 用 8.9%。
- 运行时堆主要用于板级设备驱动（~6KB 峰值），余量充足。
- 真正紧张点是 **CCM**：生产版仅 30%，但开发/自测版会飙到 ~89%。加 RTOS 任务时每个
  `RTOS_TASK_STACK` 直接吃 CCM；发布版每加 1KB 栈 CCM 占用 +1.6%，开发版基本无余量需先扩策略。
- 飞控 App 可用内存现 **91 KB**（0x20007000–0x2001DC00）。
