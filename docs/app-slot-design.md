# 方案 Y（轻量版）：RTOS 系统区 + App 区 解耦部署设计

> 目标：RTOS 系统烧录到系统分区，Rust 应用层（joc-app-rust）烧录到 App 分区，
> 两者编译期完全解耦。App 不碰裸寄存器 / 不碰 NVIC / 不碰 VTOR，所有系统能力
> 经 `app_slot_t` 服务表的函数指针拿到；App 想注册中断回调时，只填
> `app_slot_t.irq_reg[]`「注册位」并调 `slot.irq_attach()`，真实中断路由仍由
> 系统侧 `irq_manager` 兜住（与现有 `att_rust` 的 `irq_manager_attach` 同构）。
>
> 本文是结构草案 + 落地规格。代码位置：
> - 系统侧：`src/app_slot/app_slot.h` + `src/app_slot/app_slot.c`
> - Rust 侧镜像：`joc-app-rust/src/abi/app_slot.rs`
> - 链接布局：`linker/STM32F407VGTX_FLASH.ld` 的 `APP_SLOT` 段

---

## 1. 总体布局（STM32F407VGTX，无 MMU）

```
Flash 0x0800_0000 ┌─────────────────────────────┐
   (系统区)       │ RTOS 内核 + 驱动 + 控制台     │
                  │ 向量表 @0x08000000            │  ← VTOR 固定，不重定位
                  │ irq_manager 表（系统私有）     │
                  ├─────────────────────────────┤ 0x080X_XXXX
   (app_slot)     │ app_slot_t 服务表（固定地址） │  ← App 经 extern 引用，只填指针/注册位
                  │  + irq_reg[8] 回调注册位       │
                  ├─────────────────────────────┤
   (app 区)       │ libapp.a 镜像（Rust 应用层）   │  ← 单独烧录，仅引用 g_app_slot
                  │ rust_app_start() 入口          │
                  └─────────────────────────────┘
```

**关键约束（轻量版 Y 不需要 App 侧中断路由层）**：
- App 永远不写 NVIC / SCB / VTOR。
- App 只往 `app_slot_t.irq_reg[]` 填「注册位」，真实路由由系统 `irq_manager` 完成。
- 系统升级（RTOS bug 修复）只要 `RTOS_ABI_VERSION` 不变，App 镜像可直接复用；
  变了则 `build.rs`（链接期）+ 运行时 `version` 双重拦截，避免 ABI 错配总线故障。

---

## 2. app_slot_t 结构草案（系统侧定义，App 镜像同布局）

```c
#define APP_SLOT_MAGIC    0x41505053u   /* "APPS" */
#define APP_SLOT_VERSION  RTOS_ABI_VERSION
#define APP_IRQ_REG_MAX   8             /* 轻量版：App 最多注册 8 个 ISR 回调 */

/* App 提交给系统的回调注册请求（att_isr_give 这类） */
typedef struct app_irq_reg {
    uint8_t  used;        /* 1 = App 填了本槽 */
    uint8_t  irq_id;      /* TIMx_IRQn 等（系统 irq_id_t 镜像，App 只给逻辑 id） */
    uint8_t  prio_class;  /* IRQ_CLASS_KERNEL / IRQ_CLASS_ZERO_LATENCY */
    uint8_t  rt_class;    /* 0=普通, 1=硬实时（与 att_rust rt_class 一致） */
    void   (*isr_cb)(void *ctx);  /* App 提供的回调，如 att_isr_give */
    void    *ctx;         /* 通常指向 App 私有 sem */
} app_irq_reg_t;

typedef struct app_slot {
    /* ---- 头部：链接期 ABI 校验 ---- */
    uint32_t magic;        /* APP_SLOT_MAGIC */
    uint32_t version;      /* 必须等于 RTOS_ABI_VERSION */
    uint32_t reserved;

    /* ---- RTOS 内核服务指针 ---- */
    int    (*task_create)(const char *name, rtos_task_entry_t fn, void *arg,
                          uint8_t prio, uint8_t priv, uint8_t rt_class);
    void   (*msleep)(uint32_t ms);
    uint32_t (*ticks)(void);

    /* ---- IPC 服务指针 ---- */
    void * (*sem_create)(const char *name, int32_t init);
    int    (*sem_give)(void *sem);
    int    (*sem_wait)(void *sem, uint32_t to);

    /* ---- 设备服务指针（统一 device vtable 镜像）---- */
    void * (*dev_open)(const char *name, int flags);
    int    (*dev_read)(void *h, void *buf, uint32_t len);
    int    (*dev_write)(void *h, const void *buf, uint32_t len);
    int    (*dev_ioctl)(void *h, uint32_t cmd, void *arg);
    int    (*dev_close)(void *h);

    /* ---- 中断回调注册位（方案 Y 轻量版关键）---- */
    /* App 在 rust_app_start() 里填这 8 个槽，再调系统 irq_attach 完成真实路由 */
    app_irq_reg_t irq_reg[APP_IRQ_REG_MAX];

    /* ---- 系统提供的注册入口（App 调它，不直接碰 irq_manager）---- */
    int    (*irq_attach)(const app_irq_reg_t *reg);  /* 内部转 irq_manager_attach */
    int    (*irq_enable)(uint8_t irq_id);
    int    (*irq_disable)(uint8_t irq_id);

    /* ---- 生命周期 ---- */
    int    (*app_start)(void);   /* 系统调用：App 入口，返回 0=OK */
    void   (*app_stop)(void);    /* 系统调用：App 卸载钩子 */
} app_slot_t;

/* 系统在固定链接地址放一个实例，App 拿到指针即可 */
extern app_slot_t g_app_slot;   /* 链接脚本预留符号，App -DRUST_APP_LIB 经 extern 引用 */
```

> 注：`task_create` 合并了原 `rtos_task_create` / `rtos_task_create_rt` 两个签名：
> 用 `rt_class` 字段区分硬实时（=RTOS_RT_HARD 时 prio 必须 <= RTOS_PRIO_BH_HIGH），
> 避免 App 镜像两份几乎相同的函数指针。语义与 `rtos_task_attr_t` 一致。

---

## 3. App 侧填充与注册（Rust 视角）

```rust
// joc-app-rust/src/abi/app_slot.rs —— 镜像同一布局（手写，零工具链依赖）
#[repr(C)]
pub struct AppIrqReg { /* ... 同 app_irq_reg_t ... */ }

#[repr(C)]
pub struct AppSlot { /* ... 同 app_slot_t ... */ }

extern "C" { pub static mut g_app_slot: AppSlot; }  // 系统预留符号

// att_rust 的硬实时 ISR 回调：TIM 上半部只 sem_give
unsafe extern "C" fn att_isr_give(ctx: *mut c_void) {
    let sem = ctx as *mut rtos_sem_t;
    (*APP_SLOT).sem_give(sem);
}

pub extern "C" fn rust_app_start() -> i32 {
    let slot = &mut *core::ptr::addr_of_mut!(g_app_slot);
    if slot.version != RTOS_ABI_VERSION { return -1; }   // 双重防御

    // 注册硬实时中断回调：TIMx -> att_isr_give -> sem
    let reg = &mut slot.irq_reg[0];
    reg.used       = 1;
    reg.irq_id     = TIMx_IRQn;          // App 只知道逻辑 id，不碰 NVIC
    reg.prio_class = IRQ_CLASS_KERNEL;
    reg.rt_class   = 1;                  // 硬实时，与现有 att_rust 一致
    reg.isr_cb     = Some(att_isr_give);
    reg.ctx        = &raw mut ATT_SEM as *mut c_void;
    slot.irq_attach(reg);                // 系统侧转 irq_manager_attach + set_priority + enable

    // 创建任务（纯经服务表，不碰 rtos 内部）
    slot.task_create("rust_demo\0".as_ptr(), rust_task_entry, 0, RTOS_PRIO_BLINK, 1, 0);
    slot.task_create("att_rust\0".as_ptr(),  rust_attitude_loop, 0, RTOS_PRIO_BH_HIGH-1, 1, 1);
    0
}
```

---

## 4. 中断路径（att_isr_give 如何打到 App）

```
TIMx 硬件 IRQ
   → 向量表固定跳 irq_manager 统一入口（系统区，VTOR 不动）
   → irq_manager 查自己的注册表（系统私有，App 看不到）
   → 调 App 填的 isr_cb = att_isr_give(ctx)
   → att_isr_give 只做 sem_give（最快路径，无调度）
   → 返回 → 调度器按需要切到 att_rust 任务跑控制律
```

系统侧 `app_slot_irq_attach()` 实现伪码：

```c
int app_slot_irq_attach(const app_irq_reg_t *reg) {
    if (!reg || !reg->used || !reg->isr_cb) return -1;
    irq_id_t id = (irq_id_t)reg->irq_id;
    irq_manager_attach(id, reg->isr_cb, reg->ctx);
    irq_manager_set_priority(id,
        reg->rt_class ? IRQ_PRIO_KERNEL : IRQ_PRIO_NORMAL,
        reg->prio_class ? IRQ_CLASS_ZERO_LATENCY : IRQ_CLASS_KERNEL);
    irq_manager_enable(id, reg->isr_cb, reg->ctx);
    return 0;
}
```

即：App 的「注册位」只描述意图，物理接线（NVIC 编程、共享线引用计数、
优先级审计）全部由系统侧既有 `irq_manager` 完成。这与现在 `att_rust`
（C 侧 TIM ISR 直接调 `rust_att_isr_give`）相比，**把硬接线从驱动私有
改为经服务表声明式注册**，解耦更彻底，且不引入新的中断路由层。

---

## 5. 链接期契约要点

| 项 | 系统侧 | App 侧 |
|----|--------|--------|
| `g_app_slot` | 固定链接地址（ld 预留 `APP_SLOT` 段，置于 app 区前） | `extern` 引用，**不能**自己定义 |
| 版本 | `version = RTOS_ABI_VERSION` | `build.rs` 比对 `rtos_abi.h`，不一致链接失败 |
| 烧录 | 系统镜像含 `g_app_slot` 空壳（函数指针在 `app_slot_init()` 填充） | 单独烧 app 区，运行时填充 `irq_reg[]` 并调 `app_start` |
| 隔离 | 所有指针指向系统区，App 代码段只读/无 DMA | App 不持任何裸寄存器地址 |

### 落地阶段说明

- **阶段 1（本文落地）**：`g_app_slot` 与 `libapp.a` 同编进一个 ELF（仍走
  `RUST_APP_LIB` 注入式链接），但 App 改为**只经 `app_slot_t` 调系统**，不再
  直接引用 `rust_app.h` 里声明的裸 RTOS 符号。中断注册位 `irq_reg[]` 经
  `app_slot_irq_attach` 生效，C 侧 TIM 驱动改为查 `g_app_slot` 注册表而非
  硬编码 `rust_att_isr_give`。
- **阶段 2（真分区）**：把 `libapp.a` 烧到独立 Flash 区（修改链接脚本 `APP`
  内存块 + 烧录脚本），`g_app_slot` 地址固定不变，App 镜像无需重编即可换区。

---

## 6. 风险与约定

1. `app_slot_t` 字段变更必须 +`RTOS_ABI_VERSION`；Rust 侧 `build.rs` 比对拦截。
2. App 栈仍由 Rust 侧静态数组提供（主 SRAM，`#[repr(align(8))]`），不进 CCM。
3. `irq_reg[]` 上限 8：硬实时飞控通常 1~3 个定时器 + 少量 DMA 完成中断足够；
   若不够，调 `APP_IRQ_REG_MAX` 并 +版本号。
4. App 的 `isr_cb` 必须遵循 ISR 约束（只做 ISR-safe 操作：sem_give / 写内存 /
   置 flag；不调 `rtos_task_create` / `rtos_mq_init` 等调度器变更 API）。
