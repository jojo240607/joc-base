#include "rtos.h"
#include "log/log.h"
#include "log/app_log.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * 非特权任务 + SVC 系统调用门 端到端自测（从控制台 "RTOSUSR" 命令触发，
 * 并注册进 RTOSALL "usr" 条目）。
 *
 * 覆盖 docs/rtos-design.md 第 6/8 章的“内核/用户态隔离 + SVC 门 + kobj 校验”：
 *   1) 任务以非特权(priv=0)运行（CONTROL.nPRIV=1，MPU 外设区仅特权 -> 不能直接碰外设）；
 *   2) 它所有内核对象操作(sem/mq/event/mutex)都经 rtos_syscall -> SVC 门，
 *      在特权 Handler 模式执行，且 SVC 门先用 rtos_kobj_validate 校验对象指针；
 *   3) 未登记的对象指针被 SVC 门拒绝（返回 -1，绝不越权访问）；
 *   4) 跨特权唤醒：非特权任务阻塞在信号量上，由特权(主)任务 give 唤醒。
 *
 * 常态任务(priv=1)路径完全不变，故对现有 BIST/自测零回归；本自测是“按需 opt-in”
 * 的非特权能力验证。
 * ------------------------------------------------------------------------- */

typedef struct {
    int priv_ok;
    int sem_ok;
    int mq_ok;
    int ev_ok;
    int mtx_ok;
    int reject_ok;
    int hs_ok;
    int blocked_on_hs;   /* usr 已阻塞在 hs 上，主任务可跨特权唤醒 */
    int done;
    rtos_sem_t   sem;
    rtos_sem_t   hs;
    rtos_mq_t    mq;
    int          mqbuf[4];
    rtos_event_t ev;
    rtos_mutex_t mtx;
} usr_ctx_t;

static usr_ctx_t g_usr;

static void usr_entry(void *arg) {
    usr_ctx_t *c = (usr_ctx_t *)arg;
    c->priv_ok = rtos_arch_in_unpriv();   /* 应 = 1（非特权态运行） */

    /* 信号量：经 SVC 门 give/wait（对象已登记，kobj 校验通过） */
    rtos_sem_init(&c->sem, 0, 1);
    rtos_kobj_register("usr_sem", KOBJ_SEM, &c->sem);
    rtos_sem_give(&c->sem);                  /* -> SVC */
    c->sem_ok = (rtos_sem_wait(&c->sem) == 0);   /* -> SVC */

    /* 消息队列：经 SVC 门 send/recv */
    rtos_mq_init(&c->mq, c->mqbuf, sizeof(int), 4);
    rtos_kobj_register("usr_mq", KOBJ_MQ, &c->mq);
    int v = 42;
    rtos_mq_send(&c->mq, &v);                /* -> SVC */
    int got = 0;
    rtos_mq_recv(&c->mq, &got);              /* -> SVC */
    c->mq_ok = (got == 42);

    /* 事件标志：经 SVC 门 set/wait */
    rtos_event_init(&c->ev);
    rtos_kobj_register("usr_ev", KOBJ_EVENT, &c->ev);
    rtos_event_set(&c->ev, 0x2);             /* -> SVC */
    uint32_t f = rtos_event_wait(&c->ev, 0x2, 0, 1);  /* -> SVC */
    c->ev_ok = ((f & 0x2u) != 0);

    /* 互斥量：经 SVC 门 lock/unlock */
    rtos_mutex_init(&c->mtx, RTOS_PRIO_BLINK);
    rtos_kobj_register("usr_mtx", KOBJ_MUTEX, &c->mtx);
    c->mtx_ok = (rtos_mutex_lock(&c->mtx) == 0);     /* -> SVC */
    if (c->mtx_ok) c->mtx_ok = (rtos_mutex_unlock(&c->mtx) == 0);  /* -> SVC */

    /* 越权指针校验：未登记的对象指针经 SVC 门必须被拒绝（返回 -1，不越权访问） */
    int dummy = 0;
    rtos_sem_t *fake = (rtos_sem_t *)&dummy;
    c->reject_ok = (rtos_sem_wait(fake) == -1);   /* -> SVC, validate 失败 */

    /* 跨特权唤醒：本非特权任务阻塞在 hs 上，由特权(主)任务 give 唤醒 */
    rtos_sem_init(&c->hs, 0, 1);
    rtos_kobj_register("usr_hs", KOBJ_SEM, &c->hs);
    c->blocked_on_hs = 1;
    rtos_sem_wait(&c->hs);                 /* -> SVC, 阻塞；主任务 give 后继续 */
    c->hs_ok = 1;

    c->done = 1;
}

int rtos_usr_selftest(void) {
    int ok = 1;
    memset(&g_usr, 0, sizeof(g_usr));
    log_printf(app_log(), LOG_INFO, "rtos", "[USR] self-test begin (unprivileged task via SVC gate)\n");

    static uint8_t st[1024] __attribute__((aligned(8)));
    /* 创建非特权任务（priv=0），优先级高于主任务使其先跑 */
    rtos_task_create_ex("usr", usr_entry, &g_usr, (uint8_t)(RTOS_PRIO_MAIN + 1),
                        st, sizeof(st), 0);

    /* 等 usr 跑到阻塞点（hs wait），再跨特权唤醒它 */
    uint32_t waited = 0;
    while (!g_usr.blocked_on_hs && !g_usr.done && waited < 2000) {
        rtos_msleep(10); waited += 10;
    }
    rtos_sem_give(&g_usr.hs);   /* 特权(主)任务直接 give，跨特权唤醒非特权任务 */

    /* 等任务收尾 */
    waited = 0;
    while (!g_usr.done && waited < 2000) {
        rtos_msleep(10); waited += 10;
    }

    #define USR_CHK(_f, _name) do { \
        if (g_usr._f) log_printf(app_log(), LOG_INFO, "rtos", "[USR] " _name ": PASS\n"); \
        else { ok = 0; log_printf(app_log(), LOG_INFO, "rtos", "[USR] " _name ": FAIL\n"); } \
    } while (0)
    USR_CHK(priv_ok,   "runs unprivileged");
    USR_CHK(sem_ok,    "sem via SVC");
    USR_CHK(mq_ok,     "mq via SVC");
    USR_CHK(ev_ok,     "event via SVC");
    USR_CHK(mtx_ok,    "mutex via SVC");
    USR_CHK(reject_ok, "kobj validate rejects bad ptr");
    USR_CHK(hs_ok,     "cross-privilege wakeup");
    #undef USR_CHK

    log_printf(app_log(), LOG_INFO, "rtos", "[USR] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* 编译期注册：RTOSALL 会遍历该段依次执行 */
RTOS_SELFTEST_ADD("usr", rtos_usr_selftest);
