# jOS RTOS 硬实时验收与稳定性测试计划

> 状态：新建（配套实现 `src/rtos/rtos_accept.c` + 命令 `RTOSACCEPT` + host 脚本 `tools/accept_runner.py`）
> 目标：在既有功能/并发/异常注入测试之上，补齐**实时性量化验收**、**长期稳定性 soak**、**深度故障注入**三块对“硬实时操作系统”最关键但此前覆盖薄弱的验证。
> 适用：STM32F407VGTX（Cortex-M4 @168MHz，DWT CYCCNT 可用）。

---

## 0. 既有覆盖盘点（为什么需要本计划）

已有 35 个 `RTOS_SELFTEST_ADD` 条目 + HIL 工具，功能正确性覆盖充分，但：

| 维度 | 已有 | 缺口 |
|---|---|---|
| 功能性 | IPC/FPU/临界区/调度/deadline/mpu/watchdog/usr/irq/stress/robust/ostest/p4 全有 | — |
| 硬实时契约 | 单任务截止期/WCET 违约检测（正例+2 反例）、RTA 可调度性、优先级带约束、看门狗联动 | RTA **自身正确性无自测**；无**多周期任务集成验收** |
| 实时性量化 | `rtos_irq.c` 测“响应有界”（布尔判定 < 预算） | 无 **WCET 数值基线 + 跨运行退化检测**；无 **调度抖动(jitter)** 度量 |
| 稳定性 | `rtos_stress` 1.5s 压测、`rtos_fuzz` 3s 混沌 | 无 **长时 soak**（小时级/数万 tick）；无 **资源耗尽 graceful 降级**；无 **重复运行累积效应** |
| 健壮性深度 | `rtos_robust` 协作式故障恢复、中断风暴 | 无 **真实并发故障**（ISR+任务同时 fault）；无 **真实栈溢出触发**（仅测检测函数） |

本计划不直接改内核（除必要 API 暴露外），全部以**新增测试套件**形式落地，零回归。

---

## 1. 验收套件设计（`RTOSACCEPT`，文件 `src/rtos/rtos_accept.c`）

复用既有基础设施：`RTOS_TEST_RESULT("[RESULT] name: PASS|FAIL"`、`RTOS_SELFTEST_ADD`（注册为 `"accept"`，但**不进 RTOSALL 长链**，理由同 FUZZ/INV：重负载任务在长链上下文会触发 TCB/CCM 工作集脆性）、`rtos_cycle_now()`（DWT CYCCNT）、`rtos_task_create_rt`、`rtos_wcrt_compute`、`rtos_stack_check_sentinel`、`g_sched_invariant_fail`/`g_rtos_*_violation`/`g_stack_overflow` 粘性标志。

### 1.1 实时性量化层（A 组）

**A1. 周期任务集集成验收（rate-monotonic）**
- 部署 N=3 个硬实时周期任务：T={1ms,2ms,5ms}，prio 递增（均 ≤ RTOS_PRIO_BH_HIGH），各 WCET ≤ 其周期的 40%。
- 背景负载：2 个 mutex 争用任务（prio 在中/低带）持续制造干扰。
- 每硬实时任务用 DWT 测“释放→完成”响应时间，统计命中率（应 100%）与 max 响应。
- 断言：`deadline_miss==0`、`wcet_miss==0`、`g_rtos_deadline_violation==0`、max 响应 ≤ deadline。
- 打印 `[LATENCY] periodic min/avg/max` 供 host 基线采集。

**A2. 中断唤醒延迟 WCET 数据库**
- 复用 `rtos_irq.c` 思路：ISR(kernel prio)→高优任务(prio 3) 唤醒，测 K=200 次延迟。
- 记录 min/avg/max/p99（cycles），断言 max < 预算（如 5000 cycles ≈ 30µs）。
- 打印 `[LATENCY] isr_wake min/avg/max/p99`。

**A3. 调度抖动(jitter)度量**
- 同优先级两任务 ping-pong（sem 交接），测 M=500 次上下文切换延迟分布。
- 断言 jitter（max-min）有界（< 阈值），证明同优先级切换确定性。
- 打印 `[JITTER] ctx_switch min/avg/max`。

**A4. RTA 正确性自测（验证 `rtos_wcrt_compute` 本身算得对）**
- 构造 3 组 (C,T,P)：
  - 已知**可调度**：C={1,1,2}, T={4,6,12}, P={0,1,2}（Liu&Layland U≈0.69<ln2）→ 断言返回 0 且 wcrt≤T。
  - 已知**不可调度**：C={3,3}, T={4,5}, P={0,1}（U=1.35>1）→ 断言返回 >0。
  - 边界**刚好可调度**：单任务 C=5,T=5 → wcrt=5==T，断言可行。
- 不依赖真实任务池，纯函数验证，可进 RTOSALL。

### 1.2 稳定性 soak 层（B 组）

**B1. 长时 soak（默认 60s，可配）**
- 周期硬实时任务(A1 风格) + fuzz 风格随机 IPC 混合运行 T 秒。
- 每 5s 采样一次：`g_sched_invariant_fail`/`g_rtos_deadline_violation`/`g_rtos_wcet_violation`/`g_rtos_sched_invalid`/`g_stack_overflow`/空闲栈水位（主任务 `rtos_stack_check_sentinel` 自身）/tick 连续性。
- 断言：全程上述粘性标志为 0、tick 单调推进、`rtos_task_count()` 创建-删除平衡（无 TCB 泄漏：结束 count == 起始 count）。

**B2. 资源耗尽 graceful 降级**
- TCB 池打满：循环 `rtos_task_create` 直到返回错误（或到 RTOS_MAX_TASKS）；再删一个后验证可重建。
- MQ 满：满队列 `rtos_mq_send` 应返回错误而非死锁/崩溃。
- 断言：池满时系统仍可响应高优任务（心跳递增）、不 HardFault。

### 1.3 健壮性深度注入层（C 组）

**C1. 真实栈溢出触发**
- 在独立 1KB 主 SRAM 缓冲（非 CCM 关键区）做真实向下溢出（递归写穿栈底），验证 sentinel 检测置 `g_stack_overflow=1` 且系统存活（不踩相邻内存）。
- 注：仅单任务私有缓冲，不影响其它任务/内核。

**C2. 并发故障（ISR + 任务同时 fault）**
- 任务 A 在 `g_robust_fault_active` 窗口内除零；同时 ISR（TIMx）内触发一次 UDF（裸指令）。
- 断言：两故障均被恢复分支捕获、CFSR 正确、系统存活、`g_fault_cfsr` 增量 == 预期、无互相破坏。

**C3. 长临界区突破硬实时（机制正确性）**
- 背景任务持长临界区（>硬实时任务 deadline，如 10ms），硬实时任务 deadline=2ms 仍自旋完成。
- 断言：硬实时任务被检测为 `deadline_miss++`（违约检测不被长临界区静默吞掉），且 tick 仍推进、系统存活。

---

## 2. 自动化回归层（host 侧）

### 2.1 `tools/accept_runner.py`
- 循环：烧录 → `RTOSACCEPT` → `RTOSALL` → `RTOSFUZZ` → `RTOSINV`（各 N 次，默认 N=5）。
- 解析 `[LATENCY]`/`[JITTER]` 行，跨运行做**退化检测**：若某次 max 比首跑基线偏差 > 10%，报 DRIFT（警告，不致命，供趋势分析）。
- 解析 `[RESULT]`/`[SELFTEST] ALL` 行，退出码 0=全 PASS。

### 2.2 `ostest_hil.py` 扩展
- 新增正则解析 `[LATENCY] name min avg max p99` / `[JITTER] ...`，纳入报告（仅展示，不强制 FAIL，除非超硬阈值——留作后续）。

---

## 3. 落地与验证

1. 新增 `src/rtos/rtos_accept.c`，`RTOS_SELFTEST_ADD("accept", ...)`。
2. CMake：加入 `RTOS_SELFTEST` 源列表；`console.c` 增加 `RTOSACCEPT` 命令 + `g_cmds[]` 条目。
3. `rtos.h` 暴露必要 API：`rtos_task_stack_watermark`（若需）、`rtos_wcrt_compute`（已有）、A4 用纯函数无需新 API。
4. 编译 → 烧录 → `RTOSACCEPT` 单跑 → `RTOSALL` 回归 → `accept_runner.py` 连跑 N 次。
5. 验收判据：A1-A4/B1-B2/C1-C3 全 PASS，`g_sched_invariant_fail==0`，`g_rtos_*_violation==0`（A4/C3 的反例违约在自测内局部校验，不污染全局底线），跨运行无 DRIFT。

---

## 4. 与既有计划的衔接

- 本计划是 `rtos-hard-realtime-plan.md`（阶段1-4 已完成）的**验收补强**，不新增内核机制，只新增验证手段。
- 与 `rtos-test-plan.md` §6 互补：§6 覆盖功能/定时/看门狗/栈水位/覆盖；本计划补实时性量化与稳定性 soak。
- 实现文件刻意**不进 RTOSALL 长链**（同 FUZZ/INV），避免重负载在长串联后触发 TCB/CCM 工作集脆性；通过独立 `RTOSACCEPT` 命令 + `accept_runner.py` 重复运行来实现“可重复验收”。
