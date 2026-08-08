# jOS + Rust 应用层：解耦独立工程 + 驱动操作 + 飞控强实时方案

> 目标：Rust 作为**独立工程**编译成 `libapp.a` 挂进固件，通过**显式 ABI 契约**操作
> RTOS 内核与驱动，并支持编写**飞控级硬实时**软件。本文评估该设计如何落地。

---

## 0. 关键结论（先给答案）

1. **Rust 操作驱动完全可行，且不应让 Rust 直接碰寄存器**：RTOS 已有统一
   `device` 接口（`src/iface/device.h`），所有外设通过 `device_manager_get("name")`
   拿到 `device *`，经 vtable（`open/read/write/ioctl/close`）调用。Rust 只要把这套
   vtable 镜像进 ABI 契约即可——**Rust 调的是 C 函数指针，零额外开销，且天然规避
   CCM/MPU/DMA 缓冲位置等坑**（这些仍由 C 驱动层负责）。
2. **飞控级强实时已具备内核基础**：`rtos_task_create_rt` + `rtos_task_attr_t`
   `{rt_class, deadline_ticks, wcet_ticks}` 已交付，含截止期违约检测、WCET 预算、
   可调度性静态自检（WCRT）、看门狗联动。Rust 只需在 ABI 中暴露
   `rtos_task_create_rt` 即可声明硬实时任务。
3. **特权约束**：飞控关键路径（如直接读 ADC 数据寄存器、写定时器比较值）需要任务
   以 `priv=1` 运行且 MPU 外设区开放。Rust 任务创建时 `priv` 参数由 C 侧 `RTOS_TASK`
   宏指定（当前 PoC 为 `priv=1`），故 Rust 任务默认特权、可直接经 `device` vtable
   操作外设。若需非特权隔离，则 Rust 必须经 SVC 门（已验证 `rtos_ipc_in_isr` 修复），
   但飞控场景建议保持特权以降低延迟。
4. **效率零影响**：Rust 任务与 C 任务在内核眼里无区别；`device` vtable 调用是普通
   函数指针跳转，与 C 侧 `dev->vtable->read()` 完全等价。

5. **RTOS 固件保持精简：用户层 demo 全部下沉到 Rust 应用层**。
   RTOS 侧只保留系统代码 + `idle` 任务，**不编译 `task_demo.c`**；demo（LED 心跳、
   飞控姿态环等）由 `joc-app-rust` 在 `rust_app_start()` 经 ABI 契约
   `rtos_task_create` / `rtos_task_create_rt` **自行创建**。
   - RTOS 侧在 `app_main` 调用一次 `rust_app_start(ctx)`（受 `RUST_APP_LIB` 宏门控）；
   - Rust 侧用主 SRAM 静态对齐数组提供任务栈（**不进 CCM**，规避 CCMRAM 63K 限制）；
   - 控制台不再有 `DEMO`/`RUST` 命令（demo 逻辑在 Rust 层，RTOS 无需感知）。
   这样 RTOS 内核 + 驱动构成“纯净系统镜像”，用户业务完全在独立 Rust 工程中演化。

---

## 1. 现状回顾（PoC 已实现，验证通过）

- Rust 以 `staticlib`（`crate-type=["staticlib"]`、`no_std`、`panic="abort"`、
  `thumbv7em-none-eabihf`）编成 `libapp.a`。
- CMake `option(RUST_APP ON)` 内联 `cargo build --release`，产物经
  `add_library(app_rust STATIC IMPORTED)` 链接进 ELF。
- 验证：链接成功、3 个 Rust 符号进入 ELF text 段、固件烧录 OK。
- 效率结论：Rust 任务与 C 任务在内核眼里无任何区别，零运行时开销。

**痛点（驱动本次重构）**：当前 PoC 仅验证了内核 IPC（信号量），未暴露驱动接口；
ABI 靠手写 `extern "C"` + 镜像 `rtos_sem_t`，契约不显式、易漂移。

---

## 2. 目标架构（已确认：平级独立仓库）

```
<workspace>/
├── joc-rtos/                  (本仓库，内核 + 驱动 + BIST，C 主导)
│   ├── src/rtos/rtos.h            (内核公共 API，已有)
│   ├── src/iface/device.h         (统一设备接口 vtable，已有)
│   ├── src/devmgr/device_manager.h(按名查设备，已有)
│   ├── tools/abi/
│   │   ├── rtos_abi.h             (★ 显式 ABI 契约：内核 API + device 接口 + 硬实时 attr)
│   │   ├── rtos_abi_ioctl.h       (★ 各驱动私有 ioctl 命令常量集中抽取，供 Rust 镜像)
│   │   └── (可选) cbindgen.toml   (自动从 .h 生成 .rs)
│   └── CMakeLists.txt             (只“链接” libapp.a，不再内联 cargo build；RUST_APP_LIB 注入)
│
└── joc-app-rust/              (★ 平级独立 git 仓库，不与 joc-rtos 耦合源码)
    ├── Cargo.toml
    ├── rust-toolchain.toml
    ├── .cargo/config.toml
    ├── src/
    │   ├── lib.rs                 (应用任务：飞控循环、驱动操作)
    │   ├── abi.rs                 (★ 镜像 rtos_abi.h：内核 + device + rt 属性)
    │   ├── ioctl.rs               (★ 镜像 rtos_abi_ioctl.h 命令常量)
    │   └── device.rs              (★ 对 device* 的安全封装：open/read/write/ioctl)
    ├── rtos_abi.rs                (★ 由契约生成/手写)
    └── build.rs                   (可选：校验 RTOS_ABI_VERSION)
```

仓库关系：
- **两个独立 git 仓库**，平级存放（如 `d:/project/mcu/oop/joc-rtos` 与
  `d:/project/mcu/oop/joc-app-rust`），各自 commit/branch/release。
- `joc-rtos` 不依赖 `joc-app-rust` 源码，只通过 CMake 变量 `RUST_APP_LIB` 认
  `libapp.a` 的绝对路径（构建时由开发者/CI 注入）。
- `joc-app-rust` 只依赖 `joc-rtos/tools/abi/rtos_abi.h` + `rtos_abi_ioctl.h` 契约文件
  （建议把这两个文件以 submodule 或定期拷贝方式同步进 Rust 工程 `abi/` 目录），
  **不 `#include` rtos.h / device.h 内部**。
- 契约版本：`RTOS_ABI_VERSION` 在 `rtos_abi.h` 中递增；Rust 侧 `build.rs` 比对，不符
  则编译失败（强类型拦截漂移）。

---

## 3. ABI 契约设计（核心）

### 3.1 契约范围（修正：驱动 + 硬实时必须进契约）

| 范围 | 内容 | 说明 |
|------|------|------|
| **内核任务** | `rtos_task_create` / `rtos_task_create_rt` / `rtos_msleep` / `rtos_tick_count` | 创建任务（含硬实时属性） |
| **IPC** | 信号量 `sem_init/wait/give`、互斥量、消息队列、事件标志 | 任务间通信 |
| **★ 设备接口** | `device_manager_get` + `device` vtable 镜像（`open/read/write/ioctl/close/irq_id`） | **Rust 操作驱动的唯一入口** |
| **★ 硬实时属性** | `rtos_task_attr_t { rt_class, deadline_ticks, wcet_ticks }` + `RTOS_PRIO_BH_HIGH` 等优先级常量 | 声明飞控级硬实时任务 |
| **★ 违约观测** | `g_rtos_deadline_violation` 读取接口、`rtos_task_query_dl()` | 飞控自检/健康监控 |
| 不进契约 | HAL 内部、寄存器地址、链接脚本、MPU 配置 | Rust 不碰裸寄存器 |

> 修正理由：用户明确要求 Rust 业务能操作驱动、写飞控。故 `device` vtable 必须进
> 契约——这是最干净的边界：Rust 拿到的是类型安全的 `device *` 句柄，调用经 C 函数
> 指针，既零开销又规避了 Rust 直接 MMIO 带来的 CCM/MPU/DMA 风险。

### 3.2 契约文件 `tools/abi/rtos_abi.h`（含 device 镜像）

```c
/* 内核 API 子集 */
void rtos_task_create(const char*n, void(*e)(void*), void*a, u8 prio, void*st, size_t sz);
void rtos_task_create_rt(const char*n, void(*e)(void*), void*a, u8 prio,
                         void*st, size_t sz, u8 priv, const rtos_task_attr_t*attr);
void rtos_msleep(u32 ms);
u32  rtos_tick_count(void);
/* IPC */
typedef struct { u32 count; u32 limit; void* waitq; } rtos_sem_t;
void rtos_sem_init(rtos_sem_t*, u32 init, u32 limit);
i32  rtos_sem_wait(rtos_sem_t*);
void rtos_sem_give(rtos_sem_t*);
/* 硬实时属性 */
typedef struct { u8 rt_class; u32 deadline_ticks; u32 wcet_ticks; } rtos_task_attr_t;
#define RTOS_RT_HARD   1
#define RTOS_PRIO_BH_HIGH 4   /* 硬实时任务必须 prio <= 此值 */

/* 设备接口镜像（来自 device.h） */
typedef struct device device;
typedef struct { int(*open)(device*); int(*close)(device*);
                 int(*read)(device*,void*,size_t); int(*write)(device*,const void*,size_t);
                 int(*ioctl)(device*,int,void*); int(*irq_id)(device*); } deviceVtable;
struct device { const deviceVtable* vtable; int type; const char* name; int cls; };
device* device_manager_get(const char* name);

#define RTOS_ABI_VERSION 1
```

### 3.3 Rust 侧安全封装（`device.rs`）

Rust 不应直接裸调 vtable，而是封装成类型安全 API：

```rust
#[repr(C)] pub struct device { pub vtable: *const deviceVtable, /* ... */ }
#[repr(C)] pub struct deviceVtable { pub open: ..., pub read: ..., /* ... */ }

pub struct Device(*mut device);   // 所有权：从 device_manager_get 拿到
impl Device {
    pub fn open(&self) -> i32 { unsafe { (*(*self.0).vtable).open(self.0) } }
    pub fn read(&self, buf: &mut [u8]) -> i32 { /* 边界安全 */ }
    pub fn write(&self, buf: &[u8]) -> i32 { /* 边界安全 */ }
    pub fn ioctl(&self, cmd: i32, arg: *mut c_void) -> i32 { /* 类型擦除 */ }
}
```

这样飞控代码写 `let adc = Device::get("adc0")?; adc.read(&mut buf);` 既安全又零成本。

---

## 4. 飞控强实时设计（重点）

### 4.1 硬实时任务创建（Rust 侧）

```rust
// 飞控姿态环：1kHz，prio=3（≤ RTOS_PRIO_BH_HIGH=4），deadline=1ms，wcet≤400us
let attr = rtos_task_attr_t { rt_class: RTOS_RT_HARD, deadline_ticks: 1, wcet_ticks: 0 };
rtos_task_create_rt(c"attitude".as_ptr(), attitude_loop, null, 3,
                    stack, stack_sz, 1 /* priv */, &attr);
```

内核已保证：
- `prio <= RTOS_PRIO_BH_HIGH(4)` 才允许硬实时（否则裁剪到 4，已有断言）。
- tick ISR 累加 `budget_used`，超 `wcet_ticks` → `wcet_miss++`；超 `deadline_ticks` →
  `deadline_miss++`；置 `g_rtos_deadline_violation`。
- 启动时可跑 WCRT 静态自检（`rtos_sched_validate`）证明任务集可调度。
- 违约可联动 IWDG（看门狗 2s 超时，确定性故障处理）。

### 4.2 飞控典型拓扑（Rust 实现）

```
TIMx_UP IRQ (1kHz) ──top-half──> rtos_sem_give(&att_sem)   // 零延迟 ISR，仅 give
       │
       ▼ (PendSV 切换)
attitude_loop (prio=3, RT_HARD, dl=1ms):   // Rust 任务
   sem_wait(&att_sem)
   adc.read(&mut samples)                   // 经 device vtable
   // 控制律计算（纯 Rust，无分配、固定栈）
   pwm.write(&mut outputs)                  // 经 device vtable
   // 不变睡眠：下一 tick 由 sem 唤醒（精确周期）
```

要点：
- **中断上半部只做 `sem_give`**（ISR 安全，已在 BH/workqueue 踩坑验证），不写 Rust 逻辑。
- **控制律在 Rust 任务里跑**，纯计算、无堆分配、`#[repr(C)]` 数据布局可控，WCET 可静态分析。
- **驱动 IO 经 device vtable**，ADC/PWM 的 DMA 缓冲位置由 C 驱动保证（Rust 不碰）。
- **周期精度**：用 `sem_wait` 同步到 TIM IRQ，而非 `msleep`（msleep 有 tick 粒度抖动）；
  若需亚 ms 精度，可暴露 `rtos_cycle_now()`（DWT CYCCNT）供 Rust 测延迟。

### 4.3 飞控必备的额外 ABI（建议补）

| 能力 | 内核现状 | Rust ABI 暴露 |
|------|---------|--------------|
| 高精度时钟 | `rtos_cycle_now()` (DWT CYCCNT) | 暴露给 Rust 测延迟/做相位补偿 |
| 锁调度（非抢占临界区） | `rtos_lock_scheduler/unlock` | 暴露给 Rust 保护原子外设序列 |
| 违约查询 | `rtos_task_query_dl()` / `g_rtos_deadline_violation` | 暴露给 Rust 健康监控任务 |
| 看门狗联动 | `rtos_hard_rt_wdt_check()` | 可选暴露，飞控可主动武装 |

---

## 5. 构建/打包流程（解耦后）

1. **独立构建 Rust**：
   ```
   cd joc-app-rust && cargo build --release   # 产出 libapp.a
   ```
2. **RTOS 侧只链接**：
   - CMake 新增 `RUST_APP_LIB` 缓存变量（默认空），指向外部 `libapp.a` 绝对路径。
   - `if(RUST_APP_LIB)`：`add_library(app_rust STATIC IMPORTED)` + `target_link_libraries`。
   - 移除原内联 `cargo build` 的 `add_custom_command`。
3. **打包一致性**：
   - RTOS 侧 `rust_app.h` 仅保留“应用暴露给 C 的接口”（`rust_task_entry` 等）。
   - 版本校验：`rtos_abi.h` 的 `RTOS_ABI_VERSION` 与 Rust 侧常量比对，不符链接期报错。

---

## 6. 待确认问题（影响实现细节）

| # | 问题 | 状态 | 采用值 |
|---|------|------|--------|
| 1 | 设备接口进契约的范围 | **已定** | 通用 vtable 必进；驱动私有 ioctl 命令抽一份 `rtos_abi_ioctl.h` 供 Rust 镜像（含 `ADC_IOCTL_SET_CHANNEL`/`UART_IOCTL_SET_FRAMING`/`STREAM_IOCTL_SET_MODE`/`GPIO_IOCTL_TOGGLE` 等） |
| 2 | 契约生成方式 | **建议默认（待最终确认）** | 手写 `rtos_abi.rs`（零工具链依赖）；契约稳定后再上 cbindgen |
| 3 | 工程组织 | **已确认** | `joc-app-rust` 平级独立 git 仓库，与 `joc-rtos` 解耦源码 |
| 4 | 飞控特权模式 | **建议默认（待最终确认）** | 飞控关键任务 `priv=1`（特权、直接经 vtable 操作外设、最低延迟）；非关键/第三方逻辑可 `priv=0` |

> 第 2、4 项采用建议默认值即可落地；你若无异议，确认后我直接按此实现。
> 若想改（如飞控任务也要 `priv=0` 隔离、或用 cbindgen），告知即可。

---

## 7. 落地步骤（确认后执行）

1. 新建 `tools/abi/rtos_abi.h`：从 `rtos.h` + `device.h` + `device_manager.h` 抽取
   稳定子集（内核 API + device vtable 镜像 + `rtos_task_attr_t` + 优先级常量 + 版本号）。
2. 新建独立工程 `joc-app-rust/`：`abi.rs`（镜像契约）+ `device.rs`（安全封装）+
   `lib.rs`（飞控示例任务）。
3. 改造 `CMakeLists.txt`：删内联 `cargo build`，改 `RUST_APP_LIB` 注入式链接。
4. 改造 `task_demo.c` + `rust_app.h`：C 侧只引用应用暴露符号。
5. 加版本校验（链接期 `RTOS_ABI_VERSION` 比对）。
6. 构建验证：`cargo build --release` → `cmake -DRUST_APP_LIB=...` → 链接 → 烧录 →
   控制台 `RUST` 命令验证心跳 + 驱动读 + 硬实时任务 deadline_miss==0。

---

## 8. 风险与对策

- **结构体漂移**：`device` vtable 字段变 → 契约版本号 +1，Rust 侧编译失败（强类型拦截）。
- **调用约定不一致**：严格 `extern "C"` + `thumbv7em-none-eabihf`，与 C 同 ABI，无风险。
- **飞控 WCET 超标**：Rust 控制律必须避免隐式分配/浮点除法/大栈帧；用 `#[no_std]` +
  固定栈 + 整型/定点运算，WCET 可静态分析（FPU 除法已在 FPU 自测覆盖）。
- **栈/内存预算**：Rust 任务栈由 C 侧 `RTOS_TASK_STACK` 分配（放 CCM），预算可控。
- **panic 行为**：`panic="abort"` → `UDF` → 内核 fault handler，飞控可据此触发 WDT。
- **DMA 缓冲位置**：Rust 不经 device vtable 直接分配 DMA 缓冲，故 CCM/DMA 冲突风险隔离在 C 侧。

---

## 9. 结论

该方案**不影响 RTOS 执行效率**（运行时零开销），且相比 PoC 显著改善：
- **驱动可控**：Rust 经 `device` vtable 操作任意外设，零开销、零裸寄存器风险。
- **飞控可达**：硬实时内核能力（`rtos_task_create_rt` + 违约检测 + WCRT + WDT 联动）
  完整暴露给 Rust，可写 1kHz 姿态环等强实时任务。
- **工程解耦**：RTOS 与 Rust 各自独立演进；ABI 漂移在编译期被拦截。
- **安全封装**：Rust 侧 `Device` 类型把 C vtable 包成边界安全的 Rust API，兼顾性能与内存安全。
