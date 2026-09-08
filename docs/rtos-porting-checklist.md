# RTOS 平台移植参考手册（基于 ESP32C3 移植实战）

本文整理自 jOS RTOS 移植到 ESP32C3（RISC-V RV32IMC，Renode 仿真调试）过程中，
以 `RTOSALL` 自测全量通过为验收目标时踩过的坑与修复经验。供后续移植到其他
平台（新 ARM 核 / RISC-V 变体 / 其他指令集）时参考。

- 验收标准：`RTOSALL` 自测 15 项全部 PASS、串口无 FAIL、Renode 无
  `ReadDoubleWord from non existing peripheral` 之类警告。
- 调试环境：Renode 仿真（`tools/renode/*.resc` + `send_cmd_*.py` + `uart_capture.log`），
  CPU 设 `PerformanceInMips 40`；RTOSALL 全程约 2 分钟。

---

## 0. 移植总体流程

1. 建构建目标（`CMakeLists.txt` 增加 `JOC_TARGET`，定义 `MCU_DEFINE`、CPU 编译选项、
   链接脚本模板 `linker/<芯片>.ld.in`）。
2. 实现 `src/rtos/arch/<isa>/` 的移植层：`context.S`（trap/上下文切换）、`port.c`
   （时钟节拍、周期计数、调度请求、启动、SVC 门）、`riscv.h` 等。
3. 实现板级：`src/board/<芯片>.c`（设备树/外设注册）、`src/hal/<芯片>/`（UART、
   中断控制器、时钟）。
4. 用自测命令逐个验证：`RTOSBASIC` → `RTOSINV` → `RTOSDEADLINE` → `RTOSCRIT` →
   `RTOSSCHED` → `RTOSUSR` → `RTOSIPC/2` → `RTOSRR` → `RTOSBUS` → `RTOSBH` →
   `RTOSTIMER` → 最后 `RTOSALL` 全量收口。
5. 用 `RTOSMARATHON` 长跑验证稳定（看门狗/马拉松任务组）。

---

## 1. 指令集陷阱：不要用“当前特权可读寄存器”判断特权级

### 现象
`RTOSCRIT` 的 B 子项失败：
```
[CRIT] B diag: priv=0 nest0=0 spun=80006 (max=80000)
[CRIT] B crit-audit overflow before=0 after=0 (expect +1): FAIL
```
`priv=0` 表示 `arch_in_priv()` 恒返回 0，导致临界区审计
（`rtos_crit_enter_mark` / `rtos_crit_exit_audit` 的 `!arch_in_priv()` 早退）被永久禁用。

### 根因（RISC-V 规范）
`common/lock.h` 的 RISC-V `arch_in_priv()` 原实现读 `mstatus.MPP == M` 判断特权。
但 RISC-V 规范规定：**`mret` 执行时会把 `mstatus.MPP` 清为 U(0)**。
于是任务经 `mret` 进入运行后，MPP 恒为 0，任务上下文读 MPP 永远判定为“非特权”。
Cortex-M 的 `CONTROL.nPRIV` 可读，RV32I 没有“当前模式可读”的对等物。

### 修复
改为软件判定（`rtos/arch/riscv/port.c` 的 `rtos_arch_in_priv()`，`lock.h` 转发）：
- trap 处理器（中断 / SVC 上下文）运行于 M 模式 → 恒特权；
- 任务上下文按 `g_running->priv` 判定（任务特权由 `rtos_arch_apply_task_priv`
  依据它写入帧内 MPP，是权威来源，且为普通 RAM 读、U/M 两态安全）。

返回 0 时审计跳过 `mcycle` 读，规避 U 模式读 M 模式 CSR 的 Illegal instruction（mcause=2）。

### 可移植教训
- 移植层实现“是否特权 / 是否中断上下文”这类探测时，**先查目标架构规范**里
  `mret`/`iret`/`eret` 等返回指令对状态位（MPP、PS、等）的重置行为，再决定
  能否直接读状态寄存器。
- 无“当前模式可读”寄存器时，用软件跟踪：trap 上下文 + 任务 TCB 的 priv 字段，
  不要在架构无关代码里塞硬编码判断。

---

## 2. 级联假失败：子测试必须自证，不要共享 `ok` 状态

### 现象
`RTOSCRIT` 的 C 子项失败：
```
[CRIT] C diag: entry=16 inblk=2 after=16 running=main
[CRIT] C RTOS_LOCK_CEILING raise->2 restore->16: FAIL
```
诊断显示 `inblk=2`、`after=16` 全部正确——C 自身的两次断言都通过，却仍报 FAIL。

### 根因
C 与 B 共享同一个 `ok` 变量：B 失败把 `ok` 置 0 后，C 不重置 `ok`，
最终 `ok ? "PASS" : "FAIL"` 反映的是“B 之后的累积状态”，并非 C 自身结果。

### 修复
B 修复后 C 自动恢复 PASS（未改 C 逻辑）。为防复发，应保证每个子项：
- 进入时把 `ok` 重置为 1（或使用独立 `ok_b`/`ok_c`）；
- 或依赖诊断打印区分“级联污染”与“真实失败”。

### 可移植教训
给自测加子项时，先确认 `ok` 作用域；诊断日志（diag 行）是区分假失败的第一手段。
本次正是靠新增 `[CRIT] B diag` / `[CRIT] C diag` 打印才一眼锁定根因。

---

## 3. 自测不得硬编码某 SoC 的外设/寄存器假设

### 现象
`watchdog: FAIL`。

### 根因（两处平台假设）
1. 复位原因解码用例用 STM32 `RCC->CSR` 的位号（IWDGRSTF=29、WWDGRSTF=30…）。
   ESP32C3 无 RCC，`board_decode_reset_reason` 的 stub 恒返回 `POWER-ON`，
   这些用例必然全部失配。
2. 设备路径强制 `device_manager_get("iwdg0")`；ESP32C3 没有 IWDG，设备缺失 → FAIL。

### 修复（`rtos/rtos_watchdog.c`）
- 复位解码用例按平台分叉：`#if defined(ESP32C3)` 下验证 stub 契约
  （任意 CSR 都返回 `RESET_REASON_POWER`）；STM32 分支保持原用例。
- 设备路径改为可选：有 `iwdg0` 时顺带走 ioctl 刷新 + 周期定时器集成；
  无设备平台只验证喂狗计数路径（`rtos_watchdog_feed` 对空设备安全返回、仍计数）。

### 可移植教训
- 平台无关的自测代码，**验证的是抽象层契约**（如 `board_decode_reset_reason`、
  `rtos_watchdog_feed` 的计数语义），不是某个芯片的寄存器布局。
- 涉及外设存在的测试用 `device_manager_get()` 探测并分支，而不是默认“必有”。
- 平台差异一律用 `MCU_DEFINE`（如 `ESP32C3`）宏隔离，别在逻辑里写裸地址。

---

## 4. 不要在架构无关代码里用硬编码 RAM 地址“偷看”内核静态量

### 现象
Renode 持续告警：
```
[WARNING] sysbus: [cpu: 0x...] ReadDoubleWord from non existing peripheral at 0x20003558
```
（累计 2 处），RTOSALL 还会把整体结果判 FAIL。

### 根因
`rtos_selftest.c` 有两处直接按 STM32 SRAM 绝对地址 `0x20003558` 读取
`g_crit_nest`（内核 `static`），移植到 ESP32C3 后该地址不映射任何外设 → 读空 → 告警。

### 修复
- 在 `rtos/core/sched.c` 增加访问器 `uint32_t rtos_crit_nest(void)`，
  在 `rtos.h` 声明；
- 自测两处改为调用 `rtos_crit_nest()`，删掉硬编码地址。

### 可移植教训
- 内核 `static` 状态一律通过**函数访问器**暴露，禁止在平台无关代码里按地址读内存
  （ARM 与 RISC-V 的 SRAM 基址/布局完全不同）。
- 移植验收时把 Renode 的 `non existing peripheral` 警告当 FAIL 处理——它往往
  意味着代码读了目标平台不存在的地址。

---

## 5. 自测耗时/竞态：验证与收尾必须在关中断窗口内完成

（此前 RTOSTIMER 的修复，一并记录，作为时序类自测的通用经验）

### 现象
`RTOSTIMER` 偶现 `periodic: fired=7 (expect 6)`。

### 根因
在 `irq_unlock()` **之后**再判定周期回调次数并停止定时器：解锁到判定之间，
外部 tick 中断又触发一次周期回调，计数多 1——竞态。

### 修复
把“判定 + 停止”整体放进 `irq_lock()` 窗口内，锁内完成验证再解锁
（`rtos/rtos_timer.c` 的 `RTOSTIMER` periodic 段）。

### 可移植教训
- 依赖外部中断驱动的自测，判定时刻必须与被测对象处于同一原子窗口；
- 模拟器（Renode）时序与真机不同，竞态在仿真上可能更易/更难复现，
  跨平台移植后务必多轮重复运行稳定性测试（本工程曾连跑 4 次确认）。

---

## 6. 本工程“移植-验收”常用工具与命令

### 构建
```powershell
cmake --build build_esp32c3 --target jOS.elf -j 8
```

### Renode 跑自测
```powershell
renode.exe --console -e "include @d:/projects/mcu/os/joc-base/tools/renode/esp32c3_jos_selftest.resc"
```
- `esp32c3_jos_selftest.resc`：`start` → 发送命令（`send_cmd_espc3_selftest.py`，
  当前 `CMDS=["RTOSALL"]`）→ `sleep` → `quit`；
- 结果看 `tools/renode/uart_capture.log`，搜索 `[SELFTEST] ... : PASS/FAIL` 与 `ALL:`。

### 无串口定位（gdb / 脚本读内存）
- `rtos_selftest.c` 维护一组 `volatile g_dbg_crit_*`、`g_rtosall_detail[]`、
  `g_rtosall_nest_at[]`、`g_rtos_current_selftest`，卡死/失败时可用
  `tools/renode/dump_riscv_trace.py` 或调试器读 RAM 判定跑到哪一项、嵌套多深。

### 诊断打印范式
在可疑子测试里加 `[xxx] diag: ...` 打印（如 `priv=1 nest0=0 spun=80006 (max=80000)`、
`entry=16 inblk=2 after=16 running=main`），一次运行即可区分
“探测函数错 / 逻辑错 / 级联污染”。

---

## 7. 移植自查清单（新平台必查）

- [ ] `context.S`：trap 入口保存/恢复帧布局与 `RISCV_FRAME_*` 宏一致；
      `mret` 前 MPP 已按任务 priv 写好（否则特权判断/用户态会错乱）。
- [ ] `arch_in_priv()` / `arch_in_isr()`：确认目标架构“返回指令是否清状态位”，
      不依赖不可读寄存器（见 §1）。
- [ ] 时钟节拍（tick）与周期计数（`rtos_cycle_now`）：确认计数器在关中断下仍运行、
      读法符合架构（RV32I 用 `mcycle`；换架构注意 U 模式可读性）。
- [ ] 调度锁语义：RV32I 无 BASEPRI，用屏蔽 `mie.MSIE` 阻止切换但放行更高优先级 ISR；
      确认 `RTOS_MAX_ZERO_LATENCY_IRQS` 按架构能力配置（RV32I 强制 0）。
- [ ] SVC 门：U 模式 `ecall` → M 模式分发 → 返回值写回帧内 `a0`；参数传递 ABI 正确。
- [ ] 板级设备注册：UART、CLINT/PLIC（或对等中断控制器）、节拍定时器；
      看门狗等可选设备用 `device_manager_get` 探测，不假设必有。
- [ ] 链接脚本：IRAM/DRAM/FLASH 分区与实际内存布局一致；
      `.rtos_selftests` 段正确收集（否则 RTOSALL 报 0 entries）。
- [ ] 自测代码：无硬编码 RAM 地址（§4）、无 SoC 特定寄存器位假设（§3）、
      子测试 `ok` 作用域正确（§2）、时序验证在原子窗口内完成（§5）。
- [ ] 验收：`RTOSALL` 全 PASS + 无 Renode 外设告警 + 多次重复稳定。

---

## 附：本次 ESP32C3 修复涉及的文件

| 文件 | 改动 |
|------|------|
| `src/common/lock.h` | RISC-V `arch_in_priv()` 改为转发软件判定；更新注释 |
| `src/rtos/arch/riscv/port.c` | 新增 `rtos_arch_in_priv()`（trap 恒特权 + `g_running->priv`） |
| `src/rtos/arch/riscv/riscv.h` | 声明 `rtos_arch_in_priv()` |
| `src/rtos/core/sched.c` | 新增 `rtos_crit_nest()` 访问器 |
| `src/rtos/rtos.h` | 声明 `rtos_crit_nest()` |
| `src/rtos/rtos_selftest.c` | 替换两处硬编码 `0x20003558`；B/C 加 diag 打印 |
| `src/rtos/rtos_watchdog.c` | 复位解码按平台分叉；设备路径改为可选 |
| `src/rtos/rtos_timer.c` | RTOSTIMER periodic 验证收进关中断窗口 |
