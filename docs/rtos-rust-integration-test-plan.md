# Rust 调 RTOS ABI 集成测试计划

## 背景

本文档定义 jOC RTOS 与上层 Rust 应用（joc-app-rust）之间集成边界（ABI 契约）的测试方案。
目标不是重复 RTOS 自身 unit/selftest（已有 rtos_selftest、rtos_ostest、os_test 等），
而是验证 Rust 侧经 `app_slot_t` 函数指针表调用 RTOS 服务的完整路径正确性、健壮性和长期稳定性。

## 当前覆盖评估

| 集成点 | 状态 | 说明 |
|---|---|---|
| app_slot_t struct layout (ABI) | 部分覆盖 | magic+version 校验通过；无字段布局一致性检测 |
| task_create_rt (spawn tasks) | 通过 | 4 个 Rust 任务（control/sensors/telem/uplink）成功创建运行 |
| dev_get("uart0") -> write | 通过 | log_task 经 app_slot write uart0 工作 |
| dev_get("usb0") -> None -> skip | 通过 | telem 无异常降级，日志 warn usb0 not available |
| dev_get("pwm0") -> None -> skip | 通过 | control 无异常降级，actuator disabled |
| Seqlock SENSOR_SEQ | 通过 | EKF 状态持续演进 |
| Mutex EST_MTX (sem-based) | 通过 | control 写、telem 读正常 |
| .data 自拷贝 (Flash -> RAM) | 通过 | App 启动时拷贝初值正确 |
| .rust_bss 放置 (APP_RAM) | 通过 | 所有静态数据在 APP_RAM 范围内 |
| 日志 ring -> log_task -> uart0 | 通过 | 周期性 hb 日志正常输出 |
| **Device open/close 路径** | **未覆盖** | Rust 侧全部 get-only，从未经 open/close |
| **Panic -> UDF -> HardFault** | **未覆盖** | Rust 任务 panic 触发的 udf#0 后系统行为未知 |
| **Deadline violation counter** | **未覆盖** | abi.rs 已声明 g_rtos_deadline_violation / rtos_rt_violation()，但从不读取 |
| **长稳定性 (>=10min 虚拟)** | **未覆盖** | 当前最长约 40s 虚拟；CYCCNT 回绕、资源泄漏无法暴露 |
| **ABI 字段布局漂移检测** | **未覆盖** | build.rs 只比对 RTOS_ABI_VERSION 数值，不验证各字段偏移 |
| **FFI 边界注入** | **未覆盖** | NULL ptr、超界名、并发 reentrant 从未测试 |
| **自动化 CI 通过/失败** | **未覆盖** | 手动跑 resc + 人工看日志 |

## 测试分层

### 第一层：Renode 日志断言（零固件改动）

在 `tools/renode/` 下新增 `verify_app.py`，自动解析 resc 输出日志并断言：

- `assert hb_seq_monotonic(log)` — 所有 hb seq 严格递增，无跳空
- `assert ticks_advance(log, min_ticks, max_ticks)` — 虚拟 tick 推进在预期范围内
- `assert zero_faults(log)` — 日志中无 fault/diagnosis/HardFault/assert 关键字
- `assert pong_responded(log)` — 每次 PING 都有 PONG 响应
- `assert boot_completed(log)` — 包含 READY、RUST app mounted、spawned 4 tasks
- `assert telem_running(log)` — 首次 telem loop 完成日志存在

输出：PASS/FAIL + 详细原因。

**文件清单：**
- `tools/renode/verify_app.py` — 验证脚本
- `tools/renode/run_and_verify.bat` — 一键运行 resc + 验证

### 第二层：Firmware 诊断命令 `RUSTDIAG`

在 `src/console.c` 新增 `RUSTDIAG` 命令，通过 `g_app_slot` 和 extern 全局变量输出集成状态：

```
> RUSTDIAG
  g_app_slot:  magic=0x41505053  version=1
  g_rtos_deadline_violation: 0
  dev_get("uart0")=0x2000xxxx
  dev_get("usb0")=0x00000000    (正常：JOC_RENODE 下无 USB 模型)
  dev_get("pwm0")=0x00000000    (正常：回放模式不注册 PWM 驱动)
  dev_get("i2c0")=0x2000xxxx
```

**涉及修改：**
- `src/console.c` — 新增 ~30 行命令处理
- `src/console.h` — 可选：声明新函数
- Renode resc — 在测试序列中加入 RUSTDIAG 校验

### 第三层：Rust 侧 feature = `integration_test`

在 `joc-app-rust` 新增 Cargo feature，编译独立 app.bin 专门跑集成场景：

**测项 A：Device open/close 循环**
遍历设备名表 `[uart0, usb0, pwm0, i2c0, spi0, uart1]`，每个执行：
Device::open -> write(短载荷) -> Device drop (自动 close)
记录全部成功/失败。

**测项 B：msleep 精度校验**
tick_count 前后差比对 msleep(N)，验证误差 <= 2 tick。

**测项 C：Deadline violation 监控**
每循环读 `g_rtos_deadline_violation` 的 extern 值，输出到日志。
control 任务 hb 行附带 `d=<violation_count>`。

**测项 D：并发 uart0 写入**
2 个任务同时通过 log ring 输出 info!()，验证环形缓冲不损坏、内容不交叉。

**测项 E：panic injection**（`cfg(panic_test)` 编译开关）
故意 `panic!("injected")` 观察 RTOS 行为：产生 HardFault 日志、系统继续运行/或挂死。

**测项 F：CYCCNT wrap 标记**
长 soak 中检测 DWT CYCCNT 回绕，精确标记回绕点前后的调度行为。

**涉及修改（joc-app-rust）：**
- `Cargo.toml` — 新增 feature
- `src/lib.rs` — feature gate 判断
- 新增 `src/intg_test.rs` — 集成测试入口

### 第四层：CI 整合

`tools/renode/ci_runner.py`：统一入口
- 接收参数指定 resc + 预期配置
- 运行 resc
- 捕获日志
- 调用 verify_app.py 断言
- 输出 PASS/FAIL + 摘要

## 实施顺序

```
第一层：verify_app.py + run_and_verify.bat
    |  (零固件改动，产出可复用的 PASS/FAIL)
    v
第二层：RUSTDIAG 命令
    |  (~30 行 console.c，让集成状态可观测)
    v
第三层-A：Device open/close 循环
    |  (Rust 侧新增 feature)
    v
第三层-B~F：逐一覆盖
    |
    v
第四层：CI runner
```

## 风险与依赖

- 第一层无外部依赖，可立即实施
- 第二层需要重建 rel 固件（cmake -DRENODE=ON ...）
- 第三层需要 Rust app 编译链
- RENODE 编译需要 OpenOCD 路径配好（已验证：CMakeCache 有 RENODE=ON 缓存）

## 文件清单

| 文件 | 说明 |
|---|---|
| `tools/renode/verify_app.py` | 日志断言引擎 |
| `tools/renode/run_and_verify.bat` | 一键运行 resc + verify |
| `src/console.c` (+~30) | RUSTDIAG 命令 |
| (app repo) `Cargo.toml` | integration_test feature |
| (app repo) `src/intg_test.rs` | Rust 侧集成测试 |
| `tools/renode/ci_runner.py` | CI 自动化总入口 |