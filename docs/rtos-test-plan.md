# jOS RTOS 测试流程设计（基于 rtos-test.md 准则）

> 本文档把 `docs/rtos-test.md` 的「正常功能—极限压力—异常注入」三维测试准则，落地为
> 一套可直接在 STM32F407 上运行的测试流程。它复用现有 jOS RTOS 自测基建，并新增三个
> 自测模块 `RTOSBASIC` / `RTOSIPC2` / `RTOSROBUST`，补上准则里当前未覆盖的边界与鲁棒性 Case。

---

## 1. 测试架构（复用现有基建）

- **编译期段收集**：各模块用 `RTOS_SELFTEST_ADD("name", fn)` 把自测注册进
  `.rtos_selftests.*` 链接段；`rtos_selftest_run_all()`（`RTOSALL` 命令）遍历该段依次运行，
  `RTOSALL` 整体返回 PASS/FAIL。新增三个模块自动进入 `RTOSALL`，**无需改中央数组**。
- **控制台命令**：`src/console.c` 已有 `RTOSIPC/MPU/STRESS/P4/USR/RR/BUS/FPU/BH` 等命令，
  本次新增 `RTOSBASIC/RTOSIPC2/RTOSROBUST` 三个命令，既可单独跑也可被 `RTOSALL` 串联。
- **上位机收报告**：`tools/companion_test.py`（发 `BIST` 断言 `SELF-TEST: PASS`）、
  `tools/rtos_verify.py`（跑 `RTOSBUS/RTOSUSR/RTOSRR/RTOSIPC/RTOSP4/RTOSALL`）。
  最终扩展一个 `tools/rtos_test_all.py` 按 §4 顺序发命令并汇成 JUnit 报告。
- **非必要任务处理**：现有 boot 期常驻任务（如 `p4_demo` 心跳任务，prio 18，周期
  `msleep(20)` 让出）在测试时**保留**，但所有新自测都采用 **baseline 容差**（先读
  `rtos_task_count()` 基线，再按相对量断言），因此对常驻任务零耦合、结果确定。
  （若追求极端隔离，未来可加 `rtos_task_suspend` 把演示任务挂起；当前不需要。）

---

## 2. 覆盖矩阵（准则 → 现状 + 本次新增）

| 准则条目 | 现有实现 | 本次新增 |
|---|---|---|
| T01 任务数上限 | — | ✅ `RTOSBASIC`：循环创建直到 `count` 停止增长，断言触顶 `RTOS_MAX_TASKS` |
| T02 删除与资源回收 | `rtos_task_create` 复用 DEAD 槽（mem 64841707） | ✅ `RTOSBASIC`：前任务退出后新任务成功运行（证明槽复用/无泄漏） |
| T03 同优先级时间片轮转 | `RTOSRR`（`rtos_rr_selftest`） | （已由 RTOSRR 覆盖，basic 仅做轻量复检） |
| T04 优先级抢占（DWT） | `RTOSP4` 仅测 BH 唤醒延迟 | ✅ `RTOSBASIC`：DWT CYCCNT 测「高优先级就绪→抢占低优先级自旋任务」延迟 |
| T05 空闲不被饿死 | — | ✅ `RTOSBASIC`：满载(高优先级周期 yield)下低优先级任务仍推进 + tick 推进 |
| §2.2 调度锁 | `sched_lock/unlock`（lock.h） | ✅ `RTOSBASIC`：锁内（BASEPRI 屏蔽 PendSV）任何任务切换都不发生（含更高优先级），解锁后才切换 |
| §2.2 临界区嵌套 | `irq_lock/unlock`（lock.h） | ✅ `RTOSBASIC`：两层嵌套进出后中断态正确恢复 |
| §2.2 SysTick-抢占唤醒 | msleep 唤醒 | ✅ `RTOSBASIC`：高优先级阻塞 `msleep` 被节拍唤醒并立即运行 |
| S01 信号量计数边界 | `RTOSIPC` 仅基本语义 | ✅ `RTOSIPC2`：give 超 `limit` 封顶、空 `trywait` 返回 -1 |
| S03 队列满/空/溢出 | `RTOSIPC` 仅基本收发 | ✅ `RTOSIPC2`：满 `trysend`=-1、腾位后成功、空 `tryrecv`=-1、阻塞收发有序 |
| S04 多任务等同一事件 | `RTOSIPC` 仅单等待者 | ✅ `RTOSIPC2`：5 等待者 + 置位，断言实际唤醒策略（广播 or 单唤醒+公平）确定 |
| S05 从 ISR 发队列/信号量 | `RTOSBH`（BH trigger 即 ISR 安全） | ✅ `RTOSIPC2`：真实 TIM2 ISR 内 `rtos_sem_give` 唤醒高优先级等待任务 |
| §3.1 除零/UDF（UsageFault） | `RTOSMPU` 仅 MPU 越权恢复 | ✅ `RTOSROBUST`：开 `DIV_0_TRP`，触发后故障钩子跳过指令、任务存活 |
| §3.1 栈溢出检测 | `RTOSMPU` item2 哨兵检测 | ✅ `RTOSROBUST`：破坏栈底哨兵→`rtos_stack_check_sentinel` 报溢出（安全，不真破坏相邻内存） |
| §3.1 任务非法 return | `rtos_task_exit` 兜底 | ✅ `RTOSROBUST`：任务直接 return → 变 DEAD、系统存活 |
| §3.2 中断风暴/嵌套 | — | ✅ `RTOSROBUST`：TIM2 ~10kHz ISR 持续 1s 狂发信号量，系统不崩、tick 推进 |
| §3.3 死锁/超时 | `RTOSP4` 天花板防反转（正面） | ✅ `RTOSROBUST`：嵌套锁无死锁(天花板生效)+阻塞任务释放后恢复（**真死锁超时需未来 `rtos_mutex_timedlock`，见 §6**） |
| §3.3 资源耗尽不崩 | `RTOSBASIC` T01 池满 | ✅ `RTOSROBUST`：池满后再次 `create` 静默失败、系统继续运行 |
| §3.3 删阻塞任务 | —（无 `rtos_task_delete`） | ⏸ 缺口，见 §6 |
| §4 马拉松 + 看门狗 | — | ⏸ 后续阶段，见 §6 |
| §5 栈水位(0xEE 填充)/优先级边界 | — | ⏸ 后续阶段，见 §6 |
| §6 代码覆盖 | — | ⏸ 后续阶段，见 §6 |

---

## 3. 测试分组与执行流程（按准则 §7 优先级）

```
RTOSBASIC    → T01/T02/T04/T05 + §2.2 调度锁/临界区嵌套/SysTick 唤醒   （地基，先跑）
RTOSIPC2     → S01/S03/S04/S05                                      （同步通信边界）
RTOSROBUST   → §3.1 异常注入 + §3.2 中断风暴 + §3.3 安全正例          （鲁棒性）
RTOSIPC      → 既有 IPC 语义（保留）
RTOSP4       → 调度延迟/BH 有界/优先级反转/段收集                       （性能）
RTOSUSR      → 非特权 + SVC 门                                       （隔离）
RTOSMPU      → MPU 越权恢复                                          （隔离）
RTOSFPU      → FPU 上下文                                            （芯片特性）
RTOSBH       → 中断上下半部                                          （BH）
RTOSSTRESS   → 多任务并发压力                                        （压力）
RTOSALL      → 串联以上全部（编译期段收集，自动遍历）
```

执行顺序即上表顺序——先验证「任务/调度/通信」基本盘，再注入异常压边界，最后长跑。
`RTOSALL` 在 boot BIST 之后从控制台单独触发，便于陪测脚本串联。

---

## 4. 三个新模块详细设计

### 4.1 RTOSBASIC（`src/rtos/rtos_basic.c`，注册 `"basic"`）

| 子项 | 方法 | 预期 |
|---|---|---|
| T01 | 读基线 `b=rtos_task_count()`；循环 `rtos_task_create` filler（prio 20，`while(!stop) msleep`），每次创建前后比对 `count`；`count` 停止增长即池满 | `count==RTOS_MAX_TASKS`，创建数 `==RTOS_MAX_TASKS-b`；置 `stop` 让 filler 退出(变 DEAD) 后，再次 `create` 复用 DEAD 槽、`count` 不增 |
| T02 | 确保存在 DEAD 槽；`create` 一个「置 ran 标志后返回」的短任务，等其运行 | `ran==1`（证明 create 在 DEAD 槽上成功复用，无泄漏/无静默丢任务） |
| T03 | 3 个同优先级(prio 18)计数任务主动 `rtos_yield`；`msleep` 后查三计数 | 三者均 >0（轮转生效；若 `RTOS_TIME_SLICE=0` 则跳过） |
| T04 | 低优先级(prio 20)任务自旋(`while(!go)` 不 yield)；主任务记 `t0=rtos_cycle_now()` 后 `create` 高优先级(prio 6)任务并 `rtos_yield`；高优先级首行记 `t1`，算 `lat=t1-t0` | `lat` 换算 < ~100µs（与 P4 BH 延迟同量级，证明抢占不被自旋饿死） |
| T05 | 数个最高优先级(5)任务周期 `rtos_yield` 模拟满载；一个低优先级(28)计数任务并发；`msleep` 后查低优先级计数 + `tick` 推进 | 低优先级计数 >0 且 `tick` 推进（满载下非最高优先级任务仍被调度，空闲时间可得） |
| §2.2 调度锁 | 主(prio 16) `sched_lock(阈值)` 后给一个被信号量挡住的高优先级(6)任务放行并 `rtos_yield`：因 PendSV 被 BASEPRI 屏蔽，**任何任务都不切换**（高优先级也不运行）；`sched_unlock` 后该高优先级任务才运行 | 锁内 `high_ran==0`；解锁后 `high_ran==1` |
| §2.2 临界区嵌套 | `st1=irq_lock(); ...; st2=irq_lock(); ...; irq_unlock(st2)` 时 `irq_is_disabled()==1`；`irq_unlock(st1)` 后 `irq_is_disabled()==0` | 嵌套退出后中断态精确恢复，不早开 |
| §2.2 SysTick 唤醒 | 高优先级(prio 6)任务 `rtos_msleep(50)` 阻塞；主记录 `t0`，任务唤醒后记 `t1` | 任务被节拍唤醒并立即运行，`t1-t0≈50ms`，无额外手动 yield |

### 4.2 RTOSIPC2（`src/rtos/rtos_ipc2.c`，注册 `"ipc2"`）

| 子项 | 方法 | 预期 |
|---|---|---|
| S01 | `rtos_sem_init(s,0,3)`；连续 `give` 4 次；读 `s.count` | `count==3`（超上限封顶，不溢出）；空时 `trywait` 返回 -1；有许可时 `trywait==0` |
| S03 | `rtos_mq_init(q,4)`；阻塞 `send` 4 条；第 5 条 `trysend` 返回 -1；`recv` 1 条腾位后 `trysend` 成功；空 `tryrecv` 返回 -1；消费者任务阻塞 `recv` 后由生产者唤醒，校验 FIFO 顺序 | 满/空行为正确、阻塞收发有序无丢项 |
| S04 | `rtos_event_init`；5 个任务 `rtos_event_wait(e,0x1,0,1)` 阻塞；主 `rtos_event_set(e,0x1)` | 断言实际唤醒策略确定：要么 5 个全唤醒（广播），要么恰好唤醒确定优先级的那 1 个（单唤醒+公平）；记录模式，不依赖特定实现 |
| S05 | 配置 TIM2 溢出中断(~1kHz)，ISR 内 `rtos_sem_give(&s05_sem)`（ISR 安全）；高优先级(prio 6)任务阻塞等 `s05_sem` | ISR 触发后高优先级等待任务被唤醒并运行（计数器递增），证明 FromISR 唤醒正确且立即抢占 |

### 4.3 RTOSROBUST（`src/rtos/rtos_robust.c`，注册 `"robust"`）

| 子项 | 方法 | 预期 |
|---|---|---|
| §3.1 除零 | 开 `SCB->CCR.DIV_0_TRP`；子任务 `volatile int x=0; int a=1/x;`；置 `g_robust_fault_active=1` 让故障钩子跳过故障指令 | UsageFault 被捕获（`g_robust_fault_cfsr` 含 DIVBYZERO），子任务跳过指令后存活(`survived==1`)，系统继续 |
| §3.1 UDF | 子任务内联 `udf #0`；同上恢复钩子 | UNDEFINSTR 被捕获，任务跳过指令存活，系统继续 |
| §3.1 栈溢出检测 | 取当前任务栈，`rtos_stack_fill_sentinel` 后故意改写栈底魔数，调 `rtos_stack_check_sentinel` | 返回 1（检测成功），还原魔数避免误报（**仅测检测函数，不做破坏性真溢出**，避免踩坏相邻 CCM） |
| §3.1 非法 return | 子任务函数体直接 `return;`（无 `while`） | 任务变 `TASK_DEAD`，`tick` 继续推进，系统不崩 |
| §3.2 中断风暴 | TIM2 提到 ~10kHz，ISR 内 `rtos_sem_give`；持续 1s；统计 ISR 触发次数 | 系统不崩、`tick` 推进≈1000、ISR 计数 >>0（高频中断下调度器/链表不受损） |
| §3.3 嵌套锁无死锁 | L(prio 14) 持天花板(prio 5)互斥量并阻塞等信号；H(prio 6) 等同一锁 | 天花板把 L 提升到 5，H 经 handoff 拿到锁运行（无死锁，同 P4 思路） |
| §3.3 阻塞任务释放恢复 | 任务 A 持锁后阻塞在 sem；主释放锁后 A 继续 | A 标志置位，证明阻塞在 IPC 对象上的任务被正确唤醒、等待链表无悬挂 |
| §3.3 资源耗尽不崩 | 复用 T01 的「池满」状态；池满后再次 `create` | 静默失败（不崩），`tick` 继续推进、其余任务仍运行 |

> 内核改动（唯一）：在 `src/rtos/arch/cortex_m/mpu.c` 的 `rtos_fault_handler` 增加
> `g_robust_fault_active` 恢复分支——与既有 `g_mpu_test_active` 同构，仅当测试置位时跳过
> 故障指令并恢复特权；真实故障仍走 `WFI` 停机。**生产路径零回归。**

---

## 5. 自动化与上位机报告

- 各新模块沿用内核 `[SELFTEST] name: PASS/FAIL` 输出；收口处加一个 `TEST_RESULT()` 宏，
  把每条 Case 印成准则 §6 要求的统一格式：
  ```
  [RESULT] T01_CreateMaxTasks: PASS
  [RESULT] S02_MutexPriorityInherit: FAIL at line 123
  ```
- `tools/rtos_test_all.py`：按 §3 顺序发送 `RTOSBASIC/RTOSIPC2/RTOSROBUST/RTOSIPC/
  RTOSP4/RTOSUSR/RTOSMPU/RTOSFPU/RTOSBH/RTOSSTRESS`，收集 `[RESULT]` 行生成报告。
- HardFault 输出（现有 `rtos_fault_handler`）已保存 `g_fault_pc/lr/cfsr`；补充打印
  `rtos_running()->name` 以便上位机定位故障任务。

---

## 6. 已知缺口与后续

> 进度：§6.1（`rtos_task_delete`）、§6.2（`rtos_mutex_timedlock`）已完成并接入
> `RTOSROBUST`（用例 `DelBlockedTask` / `MutexTimedLock`，均发 `[RESULT]` 行，RTOSALL 已覆盖）。
> 剩余 3–6 仍待后续阶段。

1. **`rtos_task_delete`**：✅ 已完成（commit 见 git 历史）。`rtos_task_delete(t)` 从就绪/睡眠/
   等待队列摘除并置 `TASK_DEAD`，TCB 槽可被 `rtos_task_create` 复用；`t==NULL/自身` 删自身。
   已由 `RTOSROBUST` 的 `DelBlockedTask` 用例验证「队列满时发送者被删」场景——系统存活、
   队列未被破坏、被删任务变 `DEAD`。约定：不要删除仍持有互斥量的任务（owner 指针会悬挂）。
2. **`rtos_mutex_timedlock`**：✅ 已完成。基于系统节拍实现计时阻塞（任务同时挂在互斥量等待队列
   与睡眠链表上，tick ISR 到期时摘除并置 `timed_out`，unlock handoff 提前拿到锁时取消计时项）。
   已由 `RTOSROBUST` 的 `MutexTimedLock` 用例验证：空闲锁立即拿到(0)、持锁者不释放时超时返回(-1)
   且耗时≈`timeout_ms`。
3. **软件定时器 / Tick 溢出(48天翻转)**：准则 §2.5；当前 RTOS 仅 `msleep`，无软件定时器原语。⏸ 待 P2。
4. **马拉松 72h + IWDG 喂狗 + 复位原因**：准则 §4；需新增长跑任务组与看门狗集成。⏸ 待 P3。
5. **栈水位(0xEE 填充) / 优先级边界(1 tick 抢占)**：✅ 已完成（P1）。
   - 栈水位：内核新增 `rtos_stack_fill_watermark`（创建任务时把“跳过栈底哨兵区、到初始帧底”的
     未使用区填 `0xEEEEEEEE`）+ `rtos_stack_used`/`rtos_stack_free`（**自下而上**数连续 0xEE 算高水位，
     栈向低地址增长，故空闲区在底部；FreeRTOS 同构）。新增 `rtos_task_ptr(i)` 按索引取 TCB 供诊断。
     运行时零开销（只在创建/测试路径扫栈）。`RTOSROBUST` 的 `StackWatermark` 用例：一次性在栈上分配
     1400B 缓冲并真实写入（刻意不用递归——`-O2` 会把尾调用优化成单帧，掩盖真实栈用量），断言
     “占用前后水位差 grew>200B”（机制确在跟踪真实栈增长）且占用后 `free>64`（未触底），并对**所有任务**
     做水位体检：逐个报告 used/free，若某任务栈底哨兵被踩（真实溢出）以其名发 FAIL 行定位（运行时粘性
     标志 `g_stack_overflow` 也一并校验），等同于“栈溢出边界 +1 字”鲁棒性断言。
   - 优先级边界：`PrioBoundary1Tick` 用例：高优先级任务 `msleep(1)`，断言唤醒延迟量化到 `[1,2]` 节拍
     ——既不塌缩成 0（立即返回）也不溢出到 3+（严重超睡），即“1 tick 抢占”边界。
6. **代码覆盖(`-fprofile-arcs`)**：准则 §6；需 host 仿真 + gdb 收集行覆盖。⏸ 待 P4。

---

## 7. 本次交付物

- `docs/rtos-test-plan.md`（本文件）
- `src/rtos/rtos_basic.c`（RTOSBASIC）
- `src/rtos/rtos_ipc2.c`（RTOSIPC2）
- `src/rtos/rtos_robust.c`（RTOSROBUST）
- `src/rtos/arch/cortex_m/mpu.c`：新增 `g_robust_fault_active` 故障恢复分支
- `src/console.c`：新增 `RTOSBASIC/RTOSIPC2/RTOSROBUST` 三个命令（并自动进入 `RTOSALL`）
