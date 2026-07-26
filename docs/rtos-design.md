# jOS — 嵌入式实时操作系统设计文档

> 目标平台：STM32F407VGT6（Cortex-M4，128KB SRAM，8-region MPU）
> 设计原则：高实时 · 编译期可拓展 · MPU 保护 · 统一驱动接口 · 易用 IPC
> 设计参考：Zephyr（`device` 模型 / kobject+syscall / 段收集）、FreeRTOS（`FreeRTOSConfig.h` / MPU 封装 / 任务通知）

---

## 0. 现有架构里已"白送"的 RTOS 地基

本项目的驱动框架已经为引入 RTOS 铺好了路，新内核主要是"填四块新东西"，而不是推倒重来。

| 已存在的东西 | 文件 | 对 RTOS 的意义 |
|---|---|---|
| **OSAL 抽象层** | `src/osal/osal.h` | 移植接缝。注释明写"换掉 `osal_baremetal.c` 即可换内核，上层零改动" |
| **`io_xfer_t.sem` 用 `osal_sem_t`** | `src/iface/io_xfer.h` | 驱动所有阻塞点已收敛到这一个信号量——内核只需让 `osal_sem` 从"忙等"变"阻塞任务"，**驱动代码一行不用改** |
| **统一 `device` 接口 + 四大家族 + devmgr** | `src/iface/device.h`, `src/devmgr/device_manager.h` | 即 Zephyr `device_get_binding()` 的 C 等价物，直接当"统一驱动接口" |
| **平台无关 irq 框架** | `src/irq/irq.h` | ISR 统一收口到 `irq_dispatch`，调度器只需接管"ISR→唤醒任务"这一跳 |
| **`ringbuffer`（SPSC 无锁）、`bus`（pub/sub）** | `src/common/ringbuffer.h`, `src/bus/bus.h` | 现成的 IPC 积木，也是上下半部零锁传递的天然通道 |
| **MPU 头文件** | `src/cmsis/mpu_armv7.h` | 8 region 硬件支持已就位 |
| **向量表 SVC/PendSV 弱符号** | `startup_stm32f407xx.s` | 系统调用门与上下文切换钩子已预留 |
| **`systick` 走 `IRQ_CommonHandler`** | `src/drv/systick.c` | 时基 ISR 已有，调度器接管即可 |

**核心洞察**：`osal` 接缝 + `io_xfer.sem` 把所有驱动的阻塞语义统一到一个信号量上。内核只要让这个信号量"能挂起/唤醒任务"，驱动层就自动获得多任务能力，无需改动任何一个 `drv/` 文件。

---

## 1. 设计目标与原则

- **高实时**：抢占式固定优先级调度 + 可选同优先级时间片；就绪队列 O(1) 选取最高就绪任务；上下文切换走 PendSV（最低优先级异常）；关键 ISR 可抢占内核。
- **编译期可拓展**：参考 `FreeRTOSConfig.h` + Zephyr 段收集。任务/IPC 对象用宏放进专用链接段，内核启动遍历段自动收集，**加功能不碰中央数组**。
- **MPU 保护**：内核/用户态隔离（Privileged/Unprivileged），无背景区（未映射即 Fault），每任务独立栈 + 溢出哨兵（MPU subregion + 软件双重），外设区与（opt-in）内核 RAM 仅特权可访问。每任务栈 region(R4) 在上下文切换时重编程（实现见第 6 章 §6）。
- **统一驱动接口**：直接复用现有 `device` 框架，不另起炉灶。
- **易用 IPC**：信号量 / 互斥量（优先级继承）/ 消息队列 / 事件标志组 / 发布订阅，全部 OOP 化、可经 MPU 校验。
- **中断上下半部**：ISR 拆为"上半部（快速、不阻塞）"与"下半部（高优先级任务，处理耗时逻辑）"，见第 4 章。
- **平滑迁移**：保留 `osal` 接缝，新内核经 `osal_baremetal.c → osal_rtos.c` 接入，现有 BIST/companion 测试继续可用。

---

## 2. 整体分层架构

```
应用任务 (main → "main" task, net_rx, logger, bh_usb, ...)
        │  统一驱动接口(device/vtable)  +  IPC 对象(rtos_sem/mq/bus/...)
        ▼
┌─────────────────────────────────────────────────────────┐
│  jOS 内核  src/rtos/                                      │
│   sched · task · ipc(sem/mutex/mq/event/bus) · kobj registry  │
│   syscalls(SVC 门 + 对象校验) · bh(上半部/下半部调度)       │
└───────────────┬───────────────────────┬─────────────────┘
                │ arch glue              │ MPU 编程
        ┌───────▼────────┐       ┌────────▼────────┐
        │ cortex_m/port  │       │ cortex_m/mpu     │
        │ PendSV/SVC asm │       │ 8-region 表      │
        └───────┬────────┘       └────────┬─────────┘
                ▼                          ▼
        现有 irq 框架 / systick        现有 CMSIS MPU
                ▼
      现有 device 框架 + devmgr + 各 drv/hal
```

内核之下的 `irq` / `device` / `devmgr` / `drv` / `hal` **全部不动**。驱动以特权态运行，ISR 经 `irq` 框架收口；ISR 通过 `osal_sem_give` / `rtos_event_set` / `ringbuffer` 推送唤醒下半部任务。

---

## 3. 高实时调度

**调度策略**（借鉴 FreeRTOS + Zephyr）：
- 抢占式固定优先级，0 = 最高。`RTOS_MAX_PRIORITIES` 默认 32。
- 同优先级可选 Round-Robin（`RTOS_TIME_SLICE_TICKS`）。
- 就绪队列 = **位图 + 每优先级双向链表**（`uint32_t ready_bitmap` + `task_t *ready_list[32]`），选最高就绪任务 O(1)，确定性好，满足硬实时。

**时基与切换**：
- `systick` ISR 调用 `rtos_tick()`：递减延时、处理时间片、置"需调度"标志。
- 实际切换延迟到 **PendSV**（Cortex-M 最低优先级异常）执行，保证 ISR 不被切换打断（`startup` 的 `PendSV_Handler` 弱符号接管）。
- `rtos_yield()`（主动让出）、`rtos_schedule()`（抢占点）。

**TCB（实际结构体，见 `src/rtos/rtos.h`）**：
```c
typedef struct task task_t;
struct task {
    void          *sp;          /* 当前栈指针(PSP)，必须位于 offset 0（SVC 汇编直接取） */
    const char    *name;
    uint8_t        prio;        /* 有效优先级（可能被互斥量提升） */
    uint8_t        base_prio;   /* 创建时原始优先级（解锁/恢复用） */
    uint8_t        priv;        /* 1=特权(默认), 0=非特权；非特权须走 SVC 门 */
    task_state_t   state;
    uint8_t       *stack_base;
    size_t         stack_size;
    void         (*entry)(void *);
    void          *arg;
    task_t        *sched_next, *sched_prev;  /* 就绪/阻塞侵入式链表 */
    task_t        *wait_next, *wait_prev;    /* 阻塞在某对象上的等待链表 */
    uint32_t       delay_ticks;
    uint32_t       runtime;     /* 累计运行 tick（统计用） */
    void          *wait_obj;    /* 阻塞在哪个对象上（调试用） */
    uint32_t       wait_mask;   /* 事件标志等待条件（仅 event 使用） */
    uint8_t        wait_mode;   /* 1=ALL, 0=ANY */
};
```
> 说明：实现采用统一的 `rtos_*` 函数式 API，而非每对象分发的 `taskVtable` 多态；MPU 为**固定 region 表**（无每任务 `mpu` 上下文字段），故 TCB 不含 `vtable` / `mpu` 字段。

**上下文切换**（Cortex-M4，含 FPU）：
- 触发 PendSV 后保存 `r4–r11`、`s16–s31`（若 `FPCCR.LSPEN` 懒栈存则按需）、`lr(EXC_RETURN)`；恢复下一任务。
- 每任务用 **PSP**（进程栈），内核用 MSP，天然隔离。

---

## 4. 中断上半部 / 下半部（Top-Half / Bottom-Half）

> 用户需求："中断应该区分为上半部和下半部——上半部快速处理、不阻塞中断；下半部作为一个高优先级任务来处理，避免耗时任务打断中断导致系统卡死。"

这是本设计的关键加固点。下面是对该想法的仔细推演，包括它如何与现有框架咬合、以及必须想清楚的取舍。

### 4.1 动机：为什么必须拆

本项目历史上踩过一类典型坑（见记忆 `47999117`）：某驱动在 **IRQ 模式下用忙等轮询标志位**，ISR 一置标志就被自己清掉，结果 ISR 永远自旋 → 系统看起来"卡死"。根因是**在中断上下文里做了本不该在中断里做的事**。

若不拆分，所有"收数据→解协议→回写"都堆在 ISR 里，ISR 越写越长，既拖慢中断响应，又容易重入/死锁。拆成上下半部后：
- **上半部**：只做"必须立刻做、且必须原子做完"的事（清中断源、取走硬件寄存器里的数据、发出唤醒信号），通常几微秒内返回。
- **下半部**：做"可以晚一点做、且可能耗时/可能阻塞"的事（协议解析、缓冲整理、文件系统写、向别的任务发消息），跑在任务上下文，随时可被更高优先级 IRQ/任务抢占。

这样**任何单个中断都不会长时间霸占 CPU**，系统不再因某个慢 ISR 而假死。

### 4.2 上半部契约（硬规则）

上半部 = 现有 `irq` 框架注册的 ISR 回调（`irq_callback_t cb(void *ctx)`），运行在**中断上下文（Handler 模式，永远特权）**。强制约束：

- ✅ **允许**：读/清外设状态（不清会重入！）、把数据推入 **SPSC `ringbuffer`**（ISR 写 head，下半部读 tail，零锁）、`osal_sem_give` / `k_event_set` / `k_work_submit`（均为 ISR 安全、不阻塞）、DQ 一个已就绪的 work item。
- ❌ **禁止**：任何阻塞调用（`osal_sem_wait`、读消息队列、睡眠）、`malloc`（非确定性）、调用会触发 SVC 进入内核的对象校验慢路径、长时间循环。

**如何兜底**：内核提供 `arch_in_isr()`（来自 `common/lock.h`）运行态探针；阻塞入口检测到"ISR/内核未启动"上下文时**退化为忙等/非阻塞**（保活、不死锁），而非 `assert`。这是有意的宽容策略——把"误在 ISR 里阻塞"从静默死锁变成可继续运行，但定位较难。**已实现**：退化路径累加 `g_ipc_misuse` 计数（见 `core/ipc_*.c` 的 ISR/未启动分支 + `rtos_ipc_misuse_count()` 诊断 API），行为不变、便于事后定位误用；非特权任务经 SVC 门时该退化路径不触发（SVC 代表任务上下文，会正常阻塞）。

### 4.3 下半部两种机制

用户想要的是"高优先级任务"模型，本设计以它为主，再加一个省 RAM 的变体。

**(A) BH 任务（tasklet，主推，完全对应"下半部=高优先级任务"）**
```c
/* 驱动 init 时创建一个归属自己的下半部任务 */
bh_t *bh = rtos_bh_task_create("bh_usb", RTOS_PRIO_BH_HIGH, stack_1k, usb_bh_fn, ctx);
/* 上半部 ISR 里： */
usb_isr(...) {  ringbuffer_write(rb, &pkt, n);  rtos_bh_trigger(bh); /* = sem_give, ISR 安全 */ }
/* 下半部任务： */
void usb_bh_fn(void *ctx) {
    for (;;) {
        rtos_bh_wait(ctx);            /* 阻塞，被上半部唤醒 */
        while (ringbuffer_read(rb, buf, &n))  process_packet(buf, n);  /* 耗时逻辑在此 */
    }
}
```
每个需要延迟处理的驱动拥有**自己专属的下半部任务**，跑在 `RTOS_PRIO_BH_*` 高优先级带。隔离性最好、延迟可预测——符合用户"高优先级任务"的语义。

**(B) 工作队列（workqueue，省 RAM 的共享变体）**
对非 timing-critical 的零散延迟工作（日志刷盘、SD 卡后处理），用一组共享 worker 任务：`k_work_submit(work)` 从 ISR 入队，worker 任务出队执行 `work->fn`。少建任务、省栈，但多个 work 共享 worker 的优先级与栈、彼此间无隔离，且一个 work 阻塞会拖累同池其它 work。

**选择建议**：实时关键路径（USB CDC 回环、CAN 收帧、ADC 连续采样）用 (A)；非关键的杂项延迟工作用 (B)。

### 4.4 与现有框架的衔接（零改造成立点）

- **`ringbuffer`（SPSC）天然是上下半部通道**：上半部 `ringbuffer_write`（写 head），下半部 `ringbuffer_read`（写 tail），无需任何锁——这正是 `stream_device.rx_rb` 已经在用的模式，直接沿用。
- **`io_xfer.sem` 是"同步读"的唤醒点**：现有模型里调用方任务在 `read()` 中阻塞于 `io_xfer.sem`，上半部 `io_xfer_complete()` 给信号唤醒的是**调用方任务**。这与 (A) 的"驱动自有 BH 任务"是两种互补模式：
  - 同步 API：调用方阻塞等传输完成（沿用 `io_xfer`）。
  - 异步/驱动内部延迟处理：用 BH 任务（4.3-A）。
  两者共存，不冲突。
- **`bus` pub/sub 可作跨任务事件下发**：下半部处理完可向订阅者广播（本身是 IPC，见第 8 章）。
- **`osal_sem` 是上下半部的"触发器"**：上半部 `osal_sem_give`（ISR 安全），下半部 `osal_sem_wait` 在 RTOS 下变为"挂起任务"——与 4.2 的"ISR 安全"约束一致。

### 4.5 优先级与实时性预算（必须想清的取舍）

这是"仔细考虑"的重点：**下半部任务不能无脑全设最高优先级**，否则它们互相争抢、还可能饿死真正的硬实时应用任务。

- 划分**专用 BH 优先级带** `RTOS_PRIO_BH_HIGH / MED`，位于：
  - 之下：普通应用任务；
  - 之上：硬件 ISR（ISR 永远抢占任务，无需在 RTOS 优先级里表示）。
  - 之上还可保留极少量"零延迟 IRQ"（`RTOS_MAX_ZERO_LATENCY_IRQS`，对应 Zephyr `CONFIG_ZERO_LATENCY_IRQS` / FreeRTOS `configMAX_SYSCALL_INTERRUPT_PRIORITY`），这类 ISR 不被内核屏蔽，用于处理抖动最敏感的硬件。**已实现**：`RTOS_MAX_ZERO_LATENCY_IRQS` 配置 + 内核临界区统一入口 `rtos_crit_enter/exit`（`core/rtos_internal.h`）——该值 `>0` 时改用 `BASEPRI` 仅屏蔽优先级 `>=` 阈值的异常（高于阈值的零延迟 ISR 永不被屏蔽），`=0`（默认）退化为 `PRIMASK` 全局关中断，行为与此前完全一致、零回归；`rtos_start()` 在启用时会把 SysTick/PendSV 置于可被屏蔽的优先级带。启用须遵守 FreeRTOS 式契约：所有调用内核 API 的 ISR 优先级必须 `>=` 该阈值（见 `src/rtos/include/rtos_config.h` 注释）。
- **延迟预算**：
  - 上半部执行时间：须有界（建议 < 几 µs，用 `DWT CYCCNT` 在 BIST 里量）。
  - 下半部唤醒延迟 = 调度器延迟 + 所有**更高优先级 BH/任务**的剩余执行时间。因 BH 带集中在高优先级，唤醒很快，但要对 BH 任务自身的最坏执行时间心中有数。
- **BH 任务数量受 SRAM 限制**（128KB，每任务独立栈）。优先用 (A) 给实时关键驱动，其余用 (B) 共享 worker 以控栈开销。

### 4.6 MPU 视角

- 上半部跑在 **Handler 模式（特权）**，可直接碰外设——无需 MPU 介入。
- 下半部跑在 **任务模式**。**实现现状**：BH / 工作队列任务经 `rtos_task_create`（默认 `priv=1` 特权）创建，可直接碰外设（驱动零改造）；非特权隔离是"按需 opt-in"——若需严格隔离，可建 `priv=0` 的 BH 任务并令其经 `device` 接口 + SVC 门访问外设。这与项目"常态任务保持特权"的整体决策一致，RTOSUSR 自测已验证非特权路径。
- 上下半部之间的数据传递走 `ringbuffer`/消息队列（位于任务允许访问的 RAM region），不经过特权内存，避免 MPU 越权。

---

## 5. 编译期可拓展（配置 + 段收集）

**配置头**（对标 `FreeRTOSConfig.h`）：`src/rtos/rtos_config.h`
```c
#define RTOS_MAX_PRIORITIES   32
#define RTOS_TICK_HZ          1000
#define RTOS_USE_MPU          1
#define RTOS_MAX_TASKS        16
#define RTOS_TIME_SLICE       1     /* 同优先级时间片轮转（已实现，见 §1/§3） */
#define RTOS_TIME_SLICE_TICKS 5     /* 每任务连续运行 5 节拍后让出 */
#define RTOS_MAX_ZERO_LATENCY_IRQS 0 /* 零延迟 IRQ 数（0=关闭，见 §4.5；已实现） */
#define RTOS_PRIO_BH_HIGH     4     /* 下半部高优先级带 */
#define RTOS_PRIO_BH_MED      6
```

**段收集**（对标 Zephyr `.init_array`，也契合本项目已有的 `init_array` 用法）：
```c
/* RTOS_TASK(_sym, _name, _entry, _prio, _stack, _ssz, _arg) */
static uint8_t g_netrx_stack[512];
RTOS_TASK(net_rx_task, "net_rx", net_rx_entry, 5, g_netrx_stack, sizeof(g_netrx_stack), NULL);
/* RTOS_MSGQ(_sym, _name, _mq, _buf, _isz, _cap) */
static int       g_can_buf[16];
static rtos_mq_t g_can_mq;
RTOS_MSGQ(can_mq, "can_rx", &g_can_mq, g_can_buf, sizeof(int), 16);
/* RTOS_BH(_sym, _name, _prio, _stack, _ssz, _fn, _ctx) */
static uint8_t g_bh_stack[1024];
RTOS_BH(usb_bh, "bh_usb", RTOS_PRIO_BH_HIGH, g_bh_stack, sizeof(g_bh_stack), usb_bh_fn, NULL);
```
宏把 `const task_def_t` / `const ipc_def_t` / `const bh_def_t` 放进链接段 `._rtos_tasks` / `._rtos_ipc` / `._rtos_bh`。`rtos_start()` 启动时遍历段自动 `task_create` / `ipc_create` / `bh_create`。加功能无需编辑内核中央数组。

---

## 6. MPU 保护设计（8 region 预算）

Cortex-M4 MPU 仅 8 region，且无背景区（未映射即 Fault）。分配：

| Region | 内容 | 权限 | 说明 |
|---|---|---|---|
| R0 | Flash 0x08000000 (1MB) | RO + 可执行 | 所有任务共享代码 |
| R1 | SRAM 0x20000000 (128KB) | 双方 RW + XN（默认）；`RTOS_MPU_PROTECT_KERNEL_RAM=1` 时改【特权 RW，XN】 | **内核 RAM 隔离（R2）**：开启后用户任务不可直读写内核 .data/.bss/堆/其它任务栈，仅能经自身栈 region(R4)+SVC 门访问（见下，opt-in） |
| R2 | 外设 0x40000000–0x50000000 | 特权 RW，XN | **用户任务不可直访外设**，必须经驱动/SVC |
| R3 | Flash BIST 备用扇区 | 特权 RW，XN | 烧录自检写闪存不被 R0 的 RO 拦截（高编号优先） |
| R4 | 当前任务栈（每任务切换重编程） | 用户 RW + XN，subregion[0] 禁访作栈底哨兵 | **每任务栈 region（R3）已实现**：见下 |
| R5–R7 | 每任务自定义（共享缓冲等） | 按任务 | 留给应用 |

**内核/用户态隔离**：
- 内核与 ISR 跑 **Privileged**；普通任务跑 **Unprivileged**。
- 任务调驱动/IPC 内核对象 → 走 **SVC 系统调用门**（接管 `startup` 的 `SVC_Handler`）。
- 采用 Zephyr 风格的 kobject + syscall 校验精简版：SVC handler 先校验"对象指针是否登记在内核对象表 + 类型匹配"，通过才以特权执行。

**栈溢出保护**：任务栈 region 底部 subregion 设"不可访问"，溢出即 MemManage Fault，内核捕获后上报而非静默崩溃。

> ✅ **实现现状（截至本版）**：MPU 固定 region 已扩展为 R0(Flash RO-X) / R1(SRAM) / R2(外设 仅特权) / R3(Flash BIST 仅特权)，并在**上下文切换时把 R4 重编程为当前任务栈 region**（见 `src/rtos/arch/cortex_m/mpu.c` 的 `rtos_mpu_set_task_stack_region` + `port.c` 的 `rtos_arch_apply_task_priv`）：
> - **R3 每任务栈 region（已实现，默认开 `RTOS_MPU_PER_TASK_STACK`）**：R4 覆盖当前任务栈（unpriv RW + 不可执行 XN），并把最低 1/8 subregion 禁访作为**栈底溢出哨兵**。要求任务栈为 2 的幂大小且基址对齐到该大小——用 `RTOS_TASK_STACK()` 声明即可（常驻任务 main/blink/idle/bist/wq/bh 与 RTOS 各自测任务均已改用）；不满足对齐的任务栈自动退回软件哨兵（`rtos_stack_check_sentinel`），不误 fault。
> - **R2 内核 RAM 仅特权（已实现，opt-in 默认关 `RTOS_MPU_PROTECT_KERNEL_RAM`）**：开启后整块 SRAM 设为【特权 RW】，非特权任务只能经自身栈 region(R4) + SVC 门访问内存，**真正的"内核/用户态 RAM 隔离"**；该哨兵(subregion[0])与内核全局对其不可达，非特权写内核全局或溢出到 guard 即 MemManage Fault。⚠ 默认关闭原因：当前系统"常态任务保持特权"且非特权任务仍与内核共享 IPC 全局对象，开启会让非特权任务碰这些共享全局即 fault——它是为"纯用户态任务(只经 SVC 门访问内核)"场景准备的开关。其机制由 `rtos_mpu_selftest` 的隔离子测试**临时开启并验证**（写内核全局 / 写栈底 guard 均触发 MemFault 并恢复）。
> - 因此"内核/用户态隔离"现在覆盖**外设 + 每任务栈(XN) + (opt-in)内核 RAM**；栈溢出在默认配置下由软件哨兵活跃检测、在开启 R2 时由 MPU subregion 哨兵检测。SVC 门对**指针伪造**的校验始终有效。
>
> 注：8-region 预算下无法给 16 个任务各分配独立 region，故采用"每任务重编程 1 个栈 region(R4)"的折中；R5–R7 仍留给应用自定义区。

---

## 7. 统一驱动接口接入（驱动零改动）

- 驱动仍特权运行，ISR 仍经 `irq` 框架。
- 阻塞点 `io_xfer.sem` / `osal_sem_wait` 在 `osal_rtos.c` 里变成"调用 `rtos_block(cur_task, obj)`"——任务挂起；ISR `osal_sem_give` 时 `rtos_wake` 找到等此对象的任务并置就绪。
- 应用任务 `device_manager_get("uart0")` 后照旧 `dev->vtable->read(...)`；若驱动内部阻塞，任务睡眠，调度器去跑别的任务。**驱动零改动**是最大卖点。
- 配合第 4 章：驱动的"下半部"若需延迟处理，自行建 BH 任务（4.3-A），照常经 `device` 接口访问外设（此时已回到特权驱动上下文，可直接访问）。

---

## 8. 任务间通讯（IPC）

在现有积木上构建，全部 OOP + 可 MPU 校验。命名采用 `rtos_` 前缀（`rtos_sem_t` / `rtos_mutex_t` / `rtos_mq_t` / `rtos_event_t` / `rtos_bus_t`，见 `src/rtos/rtos.h`；文档早期草稿里的 `k_` 前缀为规划别名）。

1. **信号量** `rtos_sem_t`：升级现有 `osal_sem`——加阻塞/唤醒、ISR 安全（`give`）。保持 `osal_sem_*` 签名兼容。
2. **互斥量** `rtos_mutex_t`：带**优先级天花板协议**，消除优先级反转（硬实时必需）。
3. **消息队列** `rtos_mq_t`：底层用现成 `ringbuffer`，套阻塞等待（MPMC 用短临界区保护 head/tail）。
4. **事件标志组** `rtos_event_t`：32 位 bitmask，任务可等"任一/全部"置位（对标 Zephyr `k_poll` / FreeRTOS `xEventGroup`）。
5. **发布/订阅** `rtos_bus_t`：已实现为与前述四件并列的**第五个一等 IPC 原语**（见 `src/rtos/rtos.h`，实现见 `src/rtos/core/ipc_bus.c`）。它是**阻塞式 topic 邮箱 + 广播唤醒**：订阅者 `rtos_bus_wait(topic, buf)` 阻塞等待，发布者 `rtos_bus_publish(topic, data)` 把数据放进该 topic 邮箱并唤醒**所有**等待者（pub/sub 语义）；每 topic 一个固定大小邮箱，最新一条覆盖旧条；ISR 安全；非特权任务经 SVC 门（KOBJ_BUS 校验指针）使用。它与 `src/bus/bus.h` 的**同步回调式 pub/sub**（logging 多 sink 后端，零 RTOS 依赖）互补——后者不变，前者是面向任务异步解耦的 RTOS 原语。自测：`RTOSBUS` 命令 + `RTOSALL` 的 `bus` 条目。

所有 IPC 对象登记进**内核对象注册表 `kobj`**（名字 + 类型 + 指针），既支持"按名获取"，又供 SVC 校验用户态传入指针是否合法（含 `KOBJ_BUS`）。

---

## 9. 启动流程（与 `main.c` 集成）
```
Reset_Handler → data/bss 拷贝（已有）
  → 早期特权初始化：log、irq 框架、systick(仅计时，未调度)
  → board_init()：照旧创建所有 device 并登记 devmgr（不动）
  → rtos_start()：收集 ._rtos_tasks/.__rtos_bh 段建 TCB/BH、装 MPU、切到首个任务、开 SysTick
  → 原 main 主循环变成 "main" 任务（或 idle 任务）
```
现有 `companion_test` 协议应答（PONG/ECHO…）走 `d_uart->write`，不受影响；诊断走 log，不变。

---

## 10. 目录结构（实际 `src/rtos/`）
```
src/rtos/
  rtos.h                   // 公开 API: rtos_task_*/rtos_sem_*/rtos_mq_*/rtos_mutex_*/rtos_event_*/rtos_bus_*
  include/
    rtos_config.h          // 编译期配置（对标 FreeRTOSConfig.h）
  core/                    // 内核核心（按职责拆分，详见下）
    rtos_internal.h        // 内部共享头：跨 core/ 各 .c 的调度器全局 + 辅助函数
    sched.c                // 就绪位图 + 睡眠链表 + 等待队列 + PendSV 切换 + 节拍 + pend/post + 有效优先级
    task.c                 // TCB 静态池 + 初始栈帧 + 任务创建/退出 + rtos_start + 段收集实例化 + 查询 API
    kobj.c                 // 内核对象注册表（按名查找 + SVC 门指针校验）
    syscalls.c             // SVC 门分发（rtos_need_svc / rtos_svc_dispatch / rtos_svc_dispatch_entry）
    bh.c/.h                // 上半部/下半部：BH 任务 + 工作队列
    ipc_sem.c              // 信号量（含 rtos_ipc_in_isr 内部判定，供所有 ipc_*.c 共用）
    ipc_mutex.c            // 互斥量（优先级天花板协议）
    ipc_mq.c               // 消息队列（定长项环形缓冲）
    ipc_event.c            // 事件标志组（32 位 ANY/ALL）
    ipc_bus.c              // 事件总线（阻塞式 topic 邮箱 + 广播唤醒，第五个一等 IPC 原语）
  rtos_selftest.c          // IPC/FPU 自测 + RTOS_SELFTEST_ADD 段收集运行器
  rtos_stress.c            // 多任务并发压力自测
  rtos_usr.c               // 非特权 + SVC 门端到端自测（RTOSUSR）
  rtos_p4.c                // 段收集 + 收尾自测（RTOSP4）
  rtos_mpu.h               // MPU 自测声明
  arch/cortex_m/
    port.c                 // 时基/切换/yield 的 arch 胶水 + DWT 周期计数
    mpu.c/.h               // 8-region 固定编程 + MemManage 恢复（无每任务重编程）+ 栈哨兵
    context.S              // PendSV/SVC 汇编 + 寄存器保存恢复（含 FPU s16-s31）
    cortex_m.h memmap.h rtos_arch.h  // per-chip 端口旋钮 / SRAM-外设 AP 权限 / 移植契约
osal/osal_rtos.c           // 替换 osal_baremetal.c，把 osal_sem 接到内核
linker/ 追加 ._rtos_tasks / ._rtos_ipc / ._rtos_bh 段 + .rtos_selftests 段
```
> 注：核心按职责拆分进 `core/`（sched/task/kobj/syscalls/bh/ipc_*），内部跨文件符号经
> `core/rtos_internal.h` 共享；公开 API 仍在 `rtos.h`。自测合并进 `rtos_selftest.c` 等文件，
> 通过 `RTOS_SELFTEST_ADD` 链接段收集，由 `RTOSALL` 统一遍历运行。

---

## 11. 迁移路径（保住现有测试）
- `CMakeLists.txt` 加开关 `RTOS=ON/OFF`：OFF 时仍编 `osal_baremetal.c`（纯裸机，现状）；ON 时编 `osal_rtos.c` + `rtos/*`。
- 第一阶段先实现 `osal_rtos.c`（让 `osal_sem` 真正阻塞任务）+ 调度器 + 单任务（idle），先把 `main` 跑成任务，验证 BIST 仍 PASS，再逐步加 MPU、IPC、多任务、上下半部。

---

## 12. 风险与取舍
- **MPU 仅 8 region**：每任务实际独享自定义区很少。对策——Flash/外设/内核RAM 用共享固定 region，每任务只重编程自己的栈 region（+至多 1 自定义）。
- **上下文切换开销**：M4 有 FPU，用 `FPCCR.LSPEN` 懒栈存，任务不用 FPU 就不存 `s16–s31`，降低切换成本。
- **Syscall 校验开销**：对象表查表 + 指针校验须高效；当前 `kobj` 表用定长数组（`KOBJ_MAX=64`）**线性扫描**校验"指针 + 类型"，规模 64 下开销可忽略（若对象数增到数百需改哈希/索引）。
- **SRAM 128KB 上限**：每任务独立栈吃内存。对策——栈尺寸编译期显式配置；BH 任务优先给实时关键驱动，其余用共享 workqueue；必要时引导到外部 RAM（FSMC 扩展）。
- **下半部优先级爆炸**：BH 任务不能全设最高优先级。对策——专用 BH 优先级带 + 共享 workqueue 兜底（见 4.5）。

---

## 13. 实施路线（分阶段，每阶段可独立验证）
1. **P0**：`osal_rtos.c` + 裸调度器（PendSV/SVC 汇编 + 就绪位图），把 `main` 跑成第一个任务，BIST PASS。
2. **P1**：完整 IPC（sem/mutex/mq/event + `rtos_bus_t` 第五原语，均已实现）+ `kobj` 注册表（含 `KOBJ_BUS`）。
3. **P2**：MPU（固定 region + 每任务栈 region + 栈哨兵 + SVC 校验）。
4. **P3**：上半部/下半部机制（`bh.c`：BH 任务 + workqueue）+ 在 USB CDC 回环、sd_card、uart IRQ 模式下验证"驱动零改动"且不再卡死。
5. **P4**：编译期段收集（`RTOS_TASK`/`RTOS_MSGQ`/`RTOS_BH` 宏 + `._rtos_tasks`/`._rtos_ipc`/`._rtos_bh` 链接段 + `rtos_start()` 遍历 `rtos_instantiate_sections()` 自动实例化）+ 收尾自测（调度延迟 / 优先级反转 / 上半部有界性；MPU 越权 Fault 已由独立 `mpu` 条目覆盖）—— **已完成**（commit P4）。

---

## 14. 小结

核心优势是**复用 > 新建**：OSAL 接缝、`device` 统一接口、`ringbuffer`/`bus` IPC 积木、`irq` 框架、向量表钩子全部现成。新内核只需填"调度器 + MPU + SVC 门 + IPC 对象表 + 上下半部调度"五块，驱动层几乎零改动。

"上半部/下半部"是把既有 `ringbuffer`(SPSC) + `osal_sem`(ISR 安全) 升级为正式内核概念的结果：上半部延续现有 `irq` ISR 的"快进快出"契约，下半部用专属高优先级 BH 任务承接耗时逻辑——既根治了 IRQ 内忙等死锁那类历史坑，又让实时关键路径的唤醒延迟可预算、可验证。
