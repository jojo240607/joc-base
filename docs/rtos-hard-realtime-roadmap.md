# jOS RTOS 硬实时能力深化路线图（阶段性目标）

> 状态：新建（2026-08-02），作为 `rtos-hard-realtime-plan.md`（阶段1-4 机制）与
>        `rtos-acceptance-test-plan.md`（A1-A4/B1-B2/C1-C3 验收）的**后续深化清单**。
> 背景：阶段1-4 的机制与验收已全部落地并通过硬件验证（RTOSACCEPT / RTOSALL 全 PASS，
>       P2 中断延迟<10µs 断言实测 1399cyc PASS，P3 临界区有界化双证据 PASS，C2 并发
>       故障修复稳定）。本文档聚焦"从可用硬实时 → 优秀硬实时"的差距与推进顺序。
> 约束：对现有应用零侵入；新增能力默认关闭 / 默认字段 0；所有断言沿用粘性标志机制。

---

## 0. 当前已具备能力（已落地且硬件验证）

| 能力 | 状态 | 验证证据 |
|---|---|---|
| 固定优先级抢占 + O(1) 选核 | ✅ | 既有 |
| 截止期 + WCET 违约检测 | ✅ | TCB 字段 + sched.c tick 检测 |
| 临界区持锁审计（g_rtos_crit_overflow） | ✅ | P3 C3 `crit_overflow_delta=500` |
| 非抢占锁调度 + 优先级天花板 | ✅ | rtos_lock_scheduler / RTOS_LOCK_CEILING + 自测 |
| WCRT 响应时间分析 | ✅ | rtos_wcrt_compute + rtos_sched_validate |
| 中断→任务最坏延迟有界（<10µs 断言） | ✅ | P2 实测 1399cyc PASS |
| 看门狗联动违约处理 | ✅ | rtos_hard_rt_wdt_check |
| RTA / 并发故障 / soak / 栈溢出 验收 | ✅ | RTOSACCEPT A1-A4/B1-B2/C1-C3 |

**判定**：jOS 已达到"形式化严格硬实时"门槛。下一步是工程化纵深（报告→阻止闭环、动态再验证、量化对照、trace 导出）。

---

## 1. 阶段性目标清单（按优先级）

### 🔴 P0 — 必须补（可交付硬实时 RTOS 的闭环）

- **P0-1 RTOSDEADLINE 硬件实测**
  - 文档声称"✅ 已交付"，但命令尚未在硬件上实测。烧录后敲 `RTOSDEADLINE` 确认能打印
    每个硬实时任务的 deadline/wcet/miss 分解计数；纳入 `accept_runner.py` 回归序列。
- **P0-2 覆盖率数字重采（P1 收尾）**
  - 当前 `build_cov/coverage_report.txt` 是旧残留，未反映 P2/P3/C2 改动。
  - 动作：coverage 构建烧录 → `RTOSACCEPT` + `RTOSCOV` → 真实 `.gcda` 落地 → 重生成报告。
- **P0-3 临界区硬上限执行（报告→阻止闭环）**
  - 现状：`g_rtos_crit_overflow` 仅**报告**超长持锁，不**阻止**。
  - 新增 `RTOS_CRIT_KILL` 配置项（与 `RTOS_HARD_RT_KILL` 同款风格）：超预算临界区在
    `rtos_crit_exit` 时触发可配置故障处理（杀任务 / 触发 WDT / 进入安全态）。

### 🟠 P1 — 重要（让"优秀"名副其实）

- **P1-1 A1 背景负载下响应分布量化**
  - A1 现有断言 `deadline_miss==0`，补测背景 mutex 争用下的 max 响应 + p99 报告。
- **P1-2 多级中断优先级端到端延迟**
  - 测高优硬实时任务被零延迟 ISR 打断后，kernel-prio ISR 唤醒延迟仍 <10µs，验证
    BASEPRI 阈值不挡零延迟 IRQ 的同时 kernel IRQ 最坏延迟有界。
- **P1-3 动态可调度性再验证**
  - `rtos_sched_validate` 只在 `rtos_start()` 跑一次。运行时动态增删硬实时任务后 WCRT
    可能变不可行。新增 `rtos_sched_revalidate()` 命令 / 钩子，动态重算。
- **P1-4 小时级 soak 增强**
  - B1 现 60s。改为可选 1h soak + 周期 `RTOSDEADLINE` 采样，验证无 TCB 泄漏 / 无计数漂移。

### 🟡 P2 — 锦上添花（区分"优秀"与"能用"）

- **P2-1 调度轨迹 trace 导出**
  - 环形缓冲记录 `(tick, task_from, task_to, reason)`，导出供 host 离线调度性分析。
- **P2-2 优先级反转实测对照**
  - 新增 C4：低优持锁 + 中优争用 + 高优等待，对比有无 `RTOS_LOCK_CEILING` 的天数阻塞时间。
- **P2-3 IPC 阻塞最坏事延迟验收**
  - 新增 A5：高优任务阻塞在 mq 上，低优生产，测从生产到高优运行的延迟分布。

---

## 2. 推进顺序（最小代价最大化收益）

| 顺序 | 目标 | 预估 | 依赖 | 状态 |
|---|---|---|---|---|
| 1 | P0-2 覆盖率重采 | 0.5h（脚本+烧录） | 硬件已接 | ⚠️ 部分（见下） |
| 2 | P0-1 RTOSDEADLINE 硬件实测 | 10min | 硬件已接 | ✅ DONE 2026-08-02 |
| 3 | P0-3 临界区硬上限执行 | 0.5d | rtos_crit_enter/exit 审计已存在 | 待做 |
| 4 | P1-2 / P1-4 中断共存 + 优先级反转对照 | 各 0.5d | A2/C 组基础设施 | 待做 |
| 5 | P2-1 调度 trace 导出 | 1-2d | 需改内核加环形缓冲（风险最高） | 待做 |

---

## 3. 本轮（2026-08-02）进展记录

### ✅ P0-1 RTOSDEADLINE 硬件实测 — DONE
- 硬件实测 `RTOSDEADLINE` 命令：正确列出 0 个硬实时任务、0 违约（命令本身工作正常）。
- 在 `acc_a1_periodic`（A1）的硬实时任务存活期间，新增 `[DEADLINE-CHK]` 内部验证：
  正确读出 3 个 RT 任务的 class/prio/deadline/wcet/dmiss/wmiss 字段
  （acc_p0 class=1 prio=2 dl=5 wc=2；acc_p1 class=1 prio=3 dl=8 wc=3；acc_p2 class=1 prio=4 dl=20 wc=6），
  证明 RTOSDEADLINE 命令依赖的内核 API 在硬件上工作正常。
- 非插桩构建 RTOSACCEPT 全 PASS（含 A1-A4/B1-B2/C1-C3）。

### ⚠️ P0-2 覆盖率重采 — 部分完成（已知阻塞）
- **已完成**：`tools/coverage_collect.py` 的 gcov 文件名解析修复（去 `.c` 后缀 + 改用 `arm-none-eabi-gcov`）。
- **已修复**：`rtos_accept.c` 的 A2/A3/C2 在 `RTOS_COVERAGE` 构建下放宽硬实时预算断言
  （插桩放大延迟/抖动是预期的，coverage 采集不应以硬实时门槛苛求）。
- **阻塞**：coverage 构建（`-DCOVERAGE=ON`）本身在 BIST 完成后卡住，RTOS 命令循环不出现
  （BIST 跑到 pinmux PASS 后无输出，clean 重建后仍复现）。非 coverage 构建启动正常。
  根因待查（疑似 gcov 计数器在 CCM 导致 RTOS 启动期内存/时序问题，与 rtos_accept.c 改动无关）。
- **现状**：P0-2 的真实硬件 .gcda 采集暂时不可行；上次会话的覆盖率数字（整体 60.4%）仍有效，
  但反映不了 P2/P3/C2 改动。记录为已知问题，待 coverage 构建启动问题修复后重采。

### 🔧 C2 并发故障验收修复（附带）
- 去掉 `g_fault_cfsr == flt0` 脆弱差分断言：robust 恢复路径（mpu.c）在恢复后**清除** CFSR 的
  DIVBYZERO 位以保证任务干净恢复，导致全局 `g_fault_cfsr` 在 C2 退出时可能比入口少该位 → 误判 FAIL。
- 改用专用 `g_robust_fault_cfsr`（恢复钩子写入的真实 CFSR 快照）验证故障捕获，非插桩/插桩构建均稳定 PASS。
- 硬件连跑验证：RTOSACCEPT 套件全 PASS（A1-A4/B1-B2/C1-C3 全绿）。

### 下一步建议
1. 先排查 P0-2 的 coverage 构建启动卡死（独立调查，不在本路线图核心目标内但阻塞 P0-2 收尾）。
2. 推进 P0-3（临界区硬上限执行）：新增 `RTOS_CRIT_KILL` 配置，让超长临界区在退出时触发可配置故障处理。
3. 推进 P1-2 / P1-4（中断共存延迟 + 优先级反转对照），把阶段2/4 机制变成可量化验收。

---

## 3. 与既有文档衔接

- `rtos-hard-realtime-plan.md`：阶段1-4 机制（截止期/临界区/RTA/中断延迟/WDT）。
- `rtos-acceptance-test-plan.md`：A1-A4/B1-B2/C1-C3 验收套件。
- 本文档：上述机制/验收之后的**深化清单**，不重复已落地内容，只列差距与推进顺序。
- 每完成一项，在对应条目后标注 `[DONE yyyy-mm-dd]` 并补充硬件验证证据。
