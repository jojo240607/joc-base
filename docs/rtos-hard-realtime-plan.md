# jOS RTOS → 严格硬实时（Hard Real-Time）改造计划

> 状态：阶段 1/2/3/4 已全部完成并验证（RTOSALL 全 PASS，25 子项含 deadline 自测均 PASS，g_sched_invariant_fail=0）
> 目标：把当前"准硬实时 / 工业级软实时"内核升级为**形式化意义上的严格硬实时 RTOS**。
> 约束：对现有应用零侵入——新增 TCB 字段默认 0 = 非实时任务，行为完全不变；
>       所有断言/审计均为零挂起风险的粘性标志，不在临界区触发异常。

---

## 0. 现状定位（已确认代码事实）

TCB（`src/rtos/rtos.h` `struct task`）现有字段：
`sp / name / prio / base_prio / priv / state / stack_base / stack_size / entry / arg /
 sched_next,prev / wait_next,prev / delay_ticks / runtime / wait_obj / wait_mask /
 wait_mode / wait_armed / timed_out`

**缺失的硬实时契约字段**：截止期、释放时间、WCET 预算、违约计数、非抢占临界区持有标记。

调度器现状：固定优先级抢占 + 跨优先级时间片（A 方案已上）+ 互斥量优先级天花板（P4 验证）。
已具备：位图 O(1) 选最高优先级、BASEPRI 选择性屏蔽（零延迟 ISR 永不被挡）、mutex 天花板。

**结论**：当前是"工业级软实时 / 准硬实时"，足以应付多数 MCU 控制场景，但缺三块硬实时契约：
① 截止期与违约检测；② 临界区有界性（含非抢占临界区原语）；③ 可调度性静态自检。

---

## 1. 总体分阶段架构

| 阶段 | 内容 | 交付的硬实时能力 |
|---|---|---|
| **阶段 1** | 截止期 + 违约检测 | 任务可声明"N tick 内必须完成"，超了被**检测并报告** |
| **阶段 2** | 临界区有界性 | 非抢占临界区原语 + 临界区持锁时长审计 + 天花板扩展到裸锁 |
| **阶段 3** | 可调度性静态自检 | WCRT 计算 + 启动/测试时证明任务集可调度的 |
| **阶段 4** | 中断→任务端到端最坏延迟 | ISR→切换最坏路径固化 + 看门狗联动违约处理 |

每阶段独立可编译、可烧录、可回滚。

---

## 2. 阶段 1：截止期 + 违约检测（最小代价跨过"严格"门槛）

### 1.1 TCB 扩展（`rtos.h` `struct task`）
新增字段（全部默认 0 = 非实时语义不变）：
```c
uint32_t deadline_ticks;  /* 相对释放时间的截止期(0=无截止期=软任务) */
uint32_t release_tick;    /* 上次被释放/唤醒的 tick，用于 WCRT/违约计算 */
uint32_t wcet_ticks;      /* 该任务单次运行最坏预算(0=不限)，超出即 WCET 违约 */
uint32_t budget_used;     /* 当前运行窗口已用 tick（tick 里累加，释放时清零） */
volatile uint32_t deadline_miss;  /* 截止期违约计数（粘性，永不清零） */
volatile uint32_t wcet_miss;      /* WCET 预算超出计数（粘性） */
uint8_t  rt_class;        /* 0=非实时(默认) 1=硬实时 2=软实时 */
uint8_t  npls_hold;       /* 持有非抢占临界区(锁调度)标记（阶段2用） */
```

### 1.2 创建接口扩展
- 新增 `rtos_task_attr_t { deadline_ticks, wcet_ticks, rt_class }`。
- 新增 `rtos_task_create_rt(name, entry, arg, prio, stack, ssz, priv, attr)` 封装；
  旧 `rtos_task_create` / `rtos_task_create_ex` 签名不变（attr=NULL → 非实时）。
- `RTOS_TASK` 宏保持 8 参兼容；新增 `RTOS_TASK_RT` 变体带 attr。

### 1.3 tick 违约检测（`sched.c` `rtos_tick_isr`）
当前 RUNNING 任务若为硬实时（`rt_class==1`）：
- 累加 `budget_used`；若 `budget_used > wcet_ticks && wcet_ticks!=0` → `wcet_miss++`，
  触发可配置处理（默认：记录 + 置 `g_rtos_deadline_violation=1`；可选 `RTOS_HARD_RT_KILL` 杀任务）。
- 若 `g_tick - release_tick > deadline_ticks && deadline_ticks!=0` → `deadline_miss++`，同样处理。
- 任务被释放/唤醒（`ready_add` / `rtos_post` / 睡眠唤醒路径）时写
  `release_tick = g_tick; budget_used = 0`。

### 1.4 违约可观测
- 新增全局 `volatile uint32_t g_rtos_deadline_violation`（OR 所有任务违约），`rtos.h` 暴露。
- 控制台新增 `RTOSDEADLINE` 命令：打印每个硬实时任务的 deadline/wcet/miss 计数。
- RTOSALL 末尾校验 `g_rtos_deadline_violation==0`（同 `g_sched_invariant_fail` 机制）。

**阶段 1 交付**：内核能声明"这个任务必须在 N tick 内完成"，超了能被检测到并报告，而非静默错过。

---

## 3. 阶段 2：临界区有界性

### 2.1 非抢占临界区原语（`lock.h` / `rtos.h`）
- 新增 `rtos_lock_scheduler()` / `rtos_unlock_scheduler()`：**只屏蔽 PendSV（锁调度），
  不关中断、不提 BASEPRI**。用于"不可被时间片打断的原子外设序列"，让跨优先级时间片不去打断它，
  同时零延迟 ISR 仍可达。
- 运行任务持锁时置 `t->npls_hold=1`；tick 里跳过对该任务的时间片剥夺。

### 2.2 临界区持锁上限审计（复用断言机制）
- `rtos_crit_enter` 记录 `crit_enter_tick = g_tick`；`rtos_crit_exit` 时若
  `g_tick - crit_enter_tick > RTOS_CRIT_MAX_TICKS`（配置项，默认 ~2ms）→ `g_rtos_crit_overflow++`（粘性）。
- 把"低优长临界区阻塞高优"从不可见变为可测量。

### 2.3 优先级天花板扩展到裸自旋锁（opt-in）
- `RTOS_LOCK_CEILING(prio)` 宏：持锁期间把任务有效优先级顶到 `prio`，释放恢复；
  覆盖"非 mutex 共享资源"的优先级反转。默认不强制。

**阶段 2 交付**：硬实时任务可用 `rtos_lock_scheduler` 保护自己不被时间片打断；所有临界区持锁时长被审计。

---

## 4. 阶段 3：可调度性静态自检

### 3.1 响应时间分析（新增 `core/rtos_sched_analysis.c`）
实现固定优先级 WCRT：`WCRT_i = C_i + Σ_{j>p_i}(⌈WCRT_i/T_j⌉·C_j)`，
其中 `C_j = wcet_ticks[j]`、`T_j = deadline_ticks[j]`（周期）或 `wcet_ticks[j]`（非周期最坏情况）。
若 `WCRT_i > deadline_i` → 返回"不可调度"，列出违约任务。

### 3.2 运行时可调度性断言
- `rtos_start()` 末尾调 `rtos_sched_validate()`：遍历所有已创建硬实时任务跑 WCRT；
  不可调度则置 `g_rtos_sched_invalid=1` 并打印（不阻塞启动，但 RTOSALL 会 FAIL）。

### 3.3 RTOSALL 集成
- 新增自测模块 `ostest_sched`：构造已知可调度的硬实时集（验证 WCRT 通过）+
  故意不可调度集（验证 `rtos_sched_validate` 能抓出）。

**阶段 3 交付**：内核能在启动/测试时**证明**任务集在截止期内可调度的。

---

## 5. 阶段 4：中断→任务端到端最坏延迟

### 4.1 中断延迟 WCET 形式化
- ✅ 已交付：`rtos_irq.c` 的 `IRQ_WAKE_BUDGET_CYCLES` 预算上限 + P4 自测「上半部有界性」测量验证
  （关中断隔离窗口，16 次取最大，边界 <10us 断言 PASS），固化"ISR→sem_give→PendSV→切换"最坏路径上限。

### 4.2 最高优先级硬实时任务的立即抢占保证
- ✅ 已交付：`rtos_task_create_rt` 里断言 `prio <= RTOS_PRIO_BH_HIGH`（越界触发 `RTOS_SCHED_ASSERT(0)`
  并裁剪到上限），防止硬实时任务被放到会被时间片耽误的低优先级。deadline 自测三例均用 prio=3 验证。

### 4.3 看门狗联动
- ✅ 已交付：`rtos_hard_rt_wdt_check()`（sched.c）在 tick 临界区内检查
  `rtos_rt_violation() | g_rtos_crit_overflow | g_rtos_sched_invalid`，若 `RTOS_HARD_RT_WDT` 开启且
  任一非零则武装 IWDG（2s 超时）。默认关闭（避免开发期自测反例误复位）；自测返回前已恢复全局
  聚合计数，故开启时跑 RTOSALL 不会误复位。

**阶段 4 交付**：端到端延迟有上界且可验证，违约可联动看门狗做确定性故障处理。
验证：2026-08-01 烧录 build_p3，RTOSALL 25 子项全 PASS（含 deadline 自测），`g_sched_invariant_fail=0`。

---

## 6. 改造后"严格硬实时"判定对照

| 严格硬实时要求 | 现状 | 改造后 |
|---|---|---|
| 固定优先级抢占 | ✅ | ✅ |
| 截止期 + 违约检测 | ❌ | ✅ 阶段1 |
| 临界区有界/可审计 | ❌ | ✅ 阶段2 |
| 可调度性证明(WCRT≤deadline) | ❌ | ✅ 阶段3 |
| 中断→任务最坏延迟有界 | 测量值 | ✅ 阶段4 |
| 违约可预测处理(看门狗) | ❌ | ✅ 阶段4 |

---

## 7. 实施节奏与风险

| 阶段 | 改动文件 | 风险 | 验证方式 |
|---|---|---|---|
| 1 截止期+违约 | rtos.h / sched.c / task.c / console.c | 低（默认字段 0=非实时） | RTOSALL + RTOSDEADLINE + 新自测 |
| 2 临界区有界 | lock.h / rtos_internal.h / sched.c | 低（新增原语不碰旧路径） | RTOSALL 仍 PASS + crit 审计=0 |
| 3 可调度性 | 新 rtos_sched_analysis.c + rtos.h | 中（算法需自测覆盖） | ostest_sched 正反例 |
| 4 中断延迟 | port.c / watchdog.c / console.c | 低（断言+联动） | RTOSIRQ 扩展 |

**落地顺序**：阶段1 → 烧录验证（RTOSALL PASS + RTOSDEADLINE 可读）→ 阶段2 → 阶段3 → 阶段4。
每阶段独立可编译、可烧录、可回滚。
