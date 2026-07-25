#ifndef JOC_BASE_DS_SPSC_H
#define JOC_BASE_DS_SPSC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * 单生产者单消费者（SPSC）无锁变长消息队列（零拷贝内部指针）
 *
 * 设计目标：在“单生产者 ↔ 单消费者”两个执行上下文之间（典型：中断服务程序
 * ISR 与主循环 main loop）以零阻塞、无锁方式传递“带长度头的变长消息”，避免关
 * 中断 / 互斥量带来的抖动与优先级反转。
 *
 * 与旧版（定长元素、整元素拷贝）的区别：本版以“整条消息”为读写单位，天然保留
 * 消息边界；并支持零拷贝（reserve 返回内部指针、peek 返回内部指针），调用方无需
 * 中转缓冲。缓冲区内部布局与 mpsc 模块共用同一套“长度头 + 物理连续 + 回绕填充”
 * 方案，故本队列可由 mbuf（消息缓冲）直接复用。
 *
 * 消息格式（存储于内部字节环形缓冲）：
 *     ┌────────────┬──────────────────────┐
 *     │ 2 字节长度 │ 消息体（len 字节）   │
 *     │ 头(小端序) │                      │
 *     └────────────┴──────────────────────┘
 *   - 长度头为 16 位小端序，表示“消息体字节数”，不含头自身。
 *   - 长度值 0xFFFF 为“回绕填充哨兵”（标记尾部填充），不可作为合法消息长度。
 *   - 单条消息体最大 0xFFFE 字节；同时还受缓冲区容量限制。
 *
 * 零拷贝读写：
 *   - 写：reserve(len,&p) 在缓冲区内直接划出一段连续空间并返回其指针 p，调用方把
 *     消息体“就地”写入 p（无需中转缓冲），随后 commit() 一次性发布。也可用
 *     push(data,len) 走拷贝式便捷接口。
 *   - 读：peek(&p,&len) 直接返回指向消息体的指针（不拷贝、不消费），调用方处理完
 *     后 consume() 越过该消息。也可用 pop(dst,cap,&len) 走拷贝式接口。
 *   - 为支持零拷贝，本实现保证“每条消息在物理内存上连续”（绝不跨越缓冲区尾部回
 *     绕）。当尾部剩余空间不足以容纳整条消息时，生产者把消息放到缓冲区起始处，并
 *     在尾部写入填充哨兵；消费者读到哨兵即自动跳到起始处。
 *
 * ── 无锁原理 ───────────────────────────────────────────────────────────────
 *   - 采用单调递增的字节计数器 wr（写）/ rd（读），used = wr - rd。生产者只写 wr、
 *     只写“空闲区”；消费者只写 rd、只读“已发布区”。各自仅改自己的索引，互不抢占，
 *     故无需锁；用 used 与 cap 判定空/满，避免 head==tail 的歧义。
 *   - 发布顺序：生产者“先写头+体、后自增 wr”；消费者“先读 wr 判非空，后读数据，
 *     最后自增 rd”。跨上下文可见性由 volatile 索引 + 编译器屏障保证（见 .c）。
 *
 * 使用约束（SPSC 契约，调用方必须保证）：
 *   - 恰好一个生产者调用 reserve/commit/push（reserve 与 commit 必须成对）；
 *   - 恰好一个消费者调用 peek/consume/pop/next_len；
 *   - clear 仅可在无并发（或双方均停机）时调用。
 *
 * 用法：
 *   - 静态分配：调用方提供后备字节缓冲区，spsc_init 绑定（owns == false）；
 *   - 堆分配：  spsc_create(cap) 自行分配后备缓冲区（owns == true）。
 * 两者经同一套 spsc 对象接口（虚表）操作，可被派生类型覆写（多态）。
 * ------------------------------------------------------------------------- */

/* 长度头字节数（2 字节小端序） */
#define SPSC_HEADER_SIZE  2u
/* 保留标记长度值：回绕填充哨兵，不可作为合法消息长度 */
#define SPSC_LEN_SENTINEL 0xFFFFu
/* 单条消息体最大字节数（受长度头 16 位与保留标记约束；另受容量限制） */
#define SPSC_MAX_MSG      0xFFFEu

typedef struct spsc spsc;

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * ------------------------------------------------------------------------- */
struct spscFun {
    /* --- 生产者侧：零拷贝写 --- */
    /* 预留一条长度为 len 的消息空间，*pptr 返回可写入消息体的缓冲区指针。
     * 成功返回 true（此后必须 commit 或 abort）；空间不足/参数非法返回 false。*/
    bool (*reserve)(spsc *self, uint16_t len, void **pptr);
    /* 提交上一次 reserve（发布消息，推进写指针）。无待提交则返回 false。*/
    bool (*commit)(spsc *self);
    /* 放弃上一次 reserve（不发布）。*/
    void (*abort)(spsc *self);

    /* --- 生产者侧：拷贝写（便捷接口）--- */
    /* 一次性写入一条消息（内部 reserve+memcpy+commit）。满则返回 false。*/
    bool (*push)(spsc *self, const void *data, uint16_t len);

    /* --- 消费者侧：零拷贝读（仅单一消费者调用）--- */
    /* 窥视队首消息：*pptr 指向消息体，*plen 为其长度；不消费。空则返回 false。
     * 返回的指针在 consume 之前保持有效。*/
    bool (*peek)(spsc *self, const void **pptr, uint16_t *plen);
    /* 消费（越过）队首消息，推进读指针。空则返回 false。*/
    bool (*consume)(spsc *self);
    /* 查询队首消息长度（不消费）。空则返回 false。*/
    bool (*next_len)(spsc *self, uint16_t *plen);

    /* --- 消费者侧：拷贝读（便捷接口，仅单一消费者调用）--- */
    /* 取出队首消息到 dst（容量 dst_cap 字节）。*plen 回填消息长度。
     * 空返回 false；dst_cap 不足以容纳则返回 false 且不消费（*plen 已给出所需长度）。*/
    bool (*pop)(spsc *self, void *dst, size_t dst_cap, uint16_t *plen);

    /* --- 状态查询（字节粒度快照）--- */
    bool   (*is_empty)(const spsc *self); /* 无就绪消息 */
    size_t (*used)(const spsc *self);     /* 已占用字节数（含帧头/填充） */
    size_t (*space)(const spsc *self);    /* 剩余字节数 = cap - used */
    size_t (*capacity)(const spsc *self); /* 缓冲区总字节数 */

    /* 清空（仅复位指针，不擦除内容；须无并发时调用） */
    void (*clear)(spsc *self);

    /* 生命周期（对应 init / create） */
    void (*deinit)(spsc *self);
    void (*destroy)(spsc *self);
};

/* SPSC 对象定义（结构体完整可见，便于静态/栈分配） */
struct spsc {
    const struct spscFun *fun;
    uint8_t *buf;             /* 后备缓冲区（字节数组） */
    size_t   cap;             /* 缓冲区总字节数 */

    volatile size_t wr;       /* 写指针（单调递增字节数，仅生产者写） */
    volatile size_t rd;       /* 读指针（单调递增字节数，仅消费者写） */

    /* ---- 零拷贝 reserve/commit 的提交态 ---- */
    size_t   resv_foot;       /* 本次 reserve 的占用字节数（含填充） */
    bool     resv;            /* 是否存在未提交的 reserve */

    bool     owns;            /* buf 是否由本对象堆分配 */
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
spsc  *spsc_create(size_t capacity);                      /* 堆分配后备缓冲区 + 初始化 */
void   spsc_init(spsc *self, void *buf, size_t buf_size); /* 绑定用户提供的静态缓冲区 */
void   spsc_deinit(spsc *self);                           /* 对应 init，释放对象侧资源 */
void   spsc_destroy(spsc *self);                          /* 对应 create，释放堆内存 */

/* 方法表实例（由 spsc.c 提供并赋值给 self->fun） */
extern const struct spscFun spsc_fun;

#endif /* JOC_BASE_DS_SPSC_H */
