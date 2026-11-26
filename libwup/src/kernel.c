/* Threads and semaphores for apps, on the stock firmware's kernel (µITRON 4, LVC rev 7677).
 *
 * The kernel's service calls are in the firmware as small ARM entries, each followed by the address of what it
 * calls and the standard µITRON function code (0xFFFFccnn: cc = the code, nn = arguments): that table names them
 * all (docs/KERNEL.md). Used here, by the address of what they call (Thumb):
 *   acre_tsk(pk) 0x89716   act_tsk(id) 0x88BD4   ext_tsk() 0x89586   del_tsk(id) 0x9191A   dly_tsk(ms) 0x89AE4 (ARM entry)
 *   ter_tsk(id) 0x9399C (stop another task; then it can be deleted)
 *   acre_flg(pk) 0x89050   del_flg(id) 0x91756   set_flg(id, bits) 0x8ACEC   clr_flg(id, mask) 0x909E4
 *   twai_flg(id, bits, mode, &got, tmo) 0x8B4F0  (mode 1 = any of the bits; tmo -1: for ever, 0: poll)
 *   pk for a task: attributes (2 = start now), exinf, entry, priority (1 = most urgent), stack size, stack, name;
 *   for an event flag: attributes (2 = several tasks may wait), the bits at first.
 * The kernel's state is at 0xC7A88: +0x34 the running task's control block, +0x50 -> the task table (u16 highest id,
 * u16 in use, then the control blocks at +4 * id; a control block: +0x12 state (0x10 dormant), +0x2E priority,
 * +0x30 base priority, +0x38 entry, +0x3C stack, +0x40 its size).
 * A finished thread's task is deleted. Should an app end without that (it hung, a thread never returned), the
 * task stays dormant: such a one is recognised later by its stack (in memory only apps have) and used again. */
#ifdef WUP_KERNEL
/* ---- on the wup-homebrew kernel (include/wupk.h): its own tasks and semaphores, no µITRON underneath */
#include <stdlib.h>
#include "wup.h"
#include "wupk.h"
typedef uint32_t u32; typedef uint8_t u8;

__attribute__((target("arm"), noinline)) unsigned wup_irq_off(void) {
    unsigned s; __asm__ volatile("mrs %0, cpsr\n\torr r1, %0, #0xc0\n\tmsr cpsr_c, r1" : "=&r"(s) : : "r1");
    return s;
}
__attribute__((target("arm"), noinline)) void wup_irq_restore(unsigned s) { __asm__ volatile("msr cpsr_c, %0" :: "r"(s)); }

int wup_sem_new(int count, int max) { return WUPK->sem_new(count, max); }
void wup_sem_free(int sem) { WUPK->sem_free(sem); }
int wup_sem_wait(int sem, int ms) { return WUPK->sem_wait(sem, ms); }
void wup_sem_signal(int sem) { WUPK->sem_signal(sem); }

#define NTHR WUP_THREADS
static struct { void (*fn)(void *); void *arg; u8 *stack; int id, used, detached; } thr[NTHR];
static int main_task = -1;
static void entry(void *p) {
    int i = (int)(uintptr_t)p;
    thr[i].fn(thr[i].arg);
    thr[i].fn = 0;
    WUPK->task_exit();
}
int wup_thread_self(void) {
    int me = WUPK->task_self();
    for (int i = 0; i < NTHR; i++) if (thr[i].used && thr[i].id == me) return i;
    return -1;
}
void wup_thread_sleep(int ms) { WUPK->sleep(ms); }
int wup_thread_start(void (*fn)(void *), void *arg, unsigned size, int priority) {
    if (main_task < 0) main_task = WUPK->task_self();
    for (int j = 0; j < NTHR; j++) if (thr[j].used && thr[j].detached && !thr[j].fn) wup_thread_join(j, 100);   /* (detached ones that ended) */
    int i = 0;
    while (i < NTHR && thr[i].used) i++;
    if (i == NTHR) return -100;
    int pri = WUPK->task_prio(WUPK->task_self()) - priority;            /* (a higher priority argument: more urgent) */
    if (pri < 1) pri = 1;
    size = (size + 7) & ~7u; if (size < 2048) size = 2048;
    thr[i].stack = malloc(size);
    if (!thr[i].stack) return -101;
    thr[i].fn = fn; thr[i].arg = arg; thr[i].used = 1; thr[i].detached = 0;
    thr[i].id = WUPK->task_start(entry, (void *)(uintptr_t)i, thr[i].stack, size, pri, "app thread");
    if (thr[i].id < 0) { free(thr[i].stack); thr[i].used = 0; return -102; }
    return i;
}
int wup_thread_join(int t, int ms) {
    if (t < 0 || t >= NTHR || !thr[t].used) return -1;
    if (WUPK->task_join(thr[t].id, ms)) return -2;
    free(thr[t].stack);
    thr[t].used = 0;
    return 0;
}
void wup_thread_kill(int t) {
    if (t < 0 || t >= NTHR || !thr[t].used || t == wup_thread_self()) return;
    WUPK->task_kill(thr[t].id);
    WUPK->task_join(thr[t].id, 0);
    free(thr[t].stack);
    thr[t].used = 0;
}
void wup_thread_detach(int t) { if (t >= 0 && t < NTHR && thr[t].used) thr[t].detached = 1; }
unsigned wup__task(void) { return (unsigned)WUPK->task_self() + 1; }
int wup__kernel_tasks(void) { int n = 0; for (int i = 0; i < NTHR; i++) n += thr[i].used; return n; }
void wup__kernel_exit(void) {
    for (int i = 0; i < NTHR; i++) if (thr[i].used && wup_thread_join(i, 1000)) wup_thread_kill(i);
}

#else
#include <stdlib.h>
#include "wup.h"
typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
typedef struct { u32 attr; void *exinf; void (*task)(void *); u32 pri, stksz; void *stk; const char *name; } t_ctsk;
#define KER       0xc7a88u
#define ACRE_TSK  ((int (*)(const t_ctsk *))0x00089717)
#define ACT_TSK   ((int (*)(int))0x00088bd5)
#define EXT_TSK   ((void (*)(void))0x00089587)
#define DLY_TSK   ((void (*)(int))0x00089ae4)
#define DEL_TSK   ((int (*)(int))0x0009191b)
#define TER_TSK   ((int (*)(int))0x0009399d)
#define ACRE_FLG  ((int (*)(const u32 *))0x00089051)
#define DEL_FLG   ((int (*)(int))0x00091757)
#define SET_FLG   ((int (*)(int, u32))0x0008aced)
#define CLR_FLG   ((int (*)(int, u32))0x000909e5)
#define TWAI_FLG  ((int (*)(int, u32, u32, u32 *, int))0x0008b4f1)
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))

__attribute__((target("arm"), noinline)) unsigned wup_irq_off(void) {
    unsigned s; __asm__ volatile("mrs %0, cpsr\n\torr r1, %0, #0xc0\n\tmsr cpsr_c, r1" : "=&r"(s) : : "r1");
    return s;
}
__attribute__((target("arm"), noinline)) void wup_irq_restore(unsigned s) { __asm__ volatile("msr cpsr_c, %0" :: "r"(s)); }

/* ---- semaphores. The kernel has few of its own left (52, the firmware uses 48), so the semaphores apps get are
   counted here, and waiting on one is queueing up and "parking" until the one who signals wakes the first in the
   queue. Parking is one kernel event flag for everybody: a bit per thread, set to wake it. As many semaphores as
   anyone needs, and a hand-over is still two kernel calls. */
#define NTHR WUP_THREADS
#define NSEM 160
static struct { short count, max; u8 used, n, q[NTHR + 1]; } sems[NSEM];     /* q: who waits (thread index + 1; 0 = the main one), in order */
static int evt;
static int me(void);
static int park(int who, int ms) {                 /* 0: woken; else timed out */
    u32 got;
    if (evt <= 0) { u32 pk[3] = { 2, 0, 0 }; evt = ACRE_FLG(pk); if (evt <= 0) return -1; }
    if (TWAI_FLG(evt, 1u << who, 1, &got, ms < 0 ? -1 : ms)) return -1;
    CLR_FLG(evt, ~(1u << who));
    return 0;
}
static void unpark(int who) { if (evt > 0) SET_FLG(evt, 1u << who); }
int wup_sem_new(int count, int max) {
    unsigned st = wup_irq_off(); int i = 1;
    while (i < NSEM && sems[i].used) i++;
    if (i < NSEM) { sems[i].used = 1; sems[i].count = (short)count; sems[i].max = (short)(max > 30000 ? 30000 : max); sems[i].n = 0; }
    wup_irq_restore(st);
    return i < NSEM ? i : -1;
}
void wup_sem_free(int sem) { if (sem > 0 && sem < NSEM) sems[sem].used = 0; }
int wup_sem_wait(int sem, int ms) {
    if (sem <= 0 || sem >= NSEM || !sems[sem].used) return -1;
    int who = me();
    unsigned st = wup_irq_off();
    if (sems[sem].count > 0) { sems[sem].count--; wup_irq_restore(st); return 0; }
    if (!ms) { wup_irq_restore(st); return -1; }
    sems[sem].q[sems[sem].n++] = (u8)who;
    wup_irq_restore(st);
    if (!park(who, ms)) return 0;
    st = wup_irq_off();                            /* timed out: still in the queue? */
    for (int i = 0; i < sems[sem].n; i++) if (sems[sem].q[i] == who) {
        for (sems[sem].n--; i < sems[sem].n; i++) sems[sem].q[i] = sems[sem].q[i + 1];
        wup_irq_restore(st);
        return -1;
    }
    wup_irq_restore(st);
    park(who, -1);                                 /* no: someone took us out at that moment, the wake-up is on its way */
    return 0;
}
void wup_sem_signal(int sem) {
    if (sem <= 0 || sem >= NSEM || !sems[sem].used) return;
    unsigned st = wup_irq_off();
    if (sems[sem].n) {
        int who = sems[sem].q[0];
        sems[sem].n--; for (int i = 0; i < sems[sem].n; i++) sems[sem].q[i] = sems[sem].q[i + 1];
        wup_irq_restore(st);
        unpark(who);
        return;
    }
    if (sems[sem].count < sems[sem].max) sems[sem].count++;
    wup_irq_restore(st);
}

/* ---- threads */
static struct { void (*fn)(void *); void *arg; u8 *stack; u32 size; int id, done, used, detached; } thr[NTHR];
static u32 tcb(int id) { u32 t = R32(KER + 0x50); return id >= 1 && id <= R16(t) ? R32(t + 4 * (u32)id) : 0; }
/* memory only apps use (wup.h): the app area, WUP_SPARE, WUP_BIGMEM. A dormant task whose stack is there is one of ours. */
#ifdef WUP_KERNEL
static int app_ram(u32 a) { return a >= WUP_APP_BASE && a < WUP_APP_END; }
#else
static int app_ram(u32 a) { return (a >= 0x350000u && a < 0x380000u) || (a >= 0x0da000u && a < 0x136000u) || (a >= 0x190000u && a < 0x2fa400u); }
#endif
int wup__kernel_tasks(void) {                      /* (tests) how many kernel tasks apps have made so far */
    u32 tt = R32(KER + 0x50); int n = 0;
    for (int id = 1; id <= (int)R16(tt); id++) { u32 t = tcb(id); if (t && app_ram(R32(t + 0x3c)) && R32(t + 0x38) != 0) n++; }
    return n;
}
static int slot_here(void) {                       /* whose stack are we on? */
    u32 sp = (u32)__builtin_frame_address(0);
    for (int i = 0; i < NTHR; i++) if (thr[i].used && sp >= (u32)thr[i].stack && sp < (u32)thr[i].stack + thr[i].size) return i;
    return -1;
}
__attribute__((target("thumb"), noinline)) static void entry(void *exinf) {
    (void)exinf;
    int i = slot_here();
    if (i >= 0) { thr[i].fn(thr[i].arg); thr[i].fn = 0; wup_sem_signal(thr[i].done); }
    EXT_TSK();                                     /* (never returns) */
}
int wup_thread_self(void) { return slot_here(); }
static int me(void) { return slot_here() + 1; }
void wup_thread_sleep(int ms) { while (ms > 0) { DLY_TSK(ms > 100 ? 100 : ms); ms -= 100; } }
int wup_thread_start(void (*fn)(void *), void *arg, unsigned size, int priority) {
    u32 cur = R32(KER + 0x34);
    int i = 0, pri = cur ? (int)R16(cur + 0x2e) - priority : 0, id = 0;
    for (int j = 0; j < NTHR; j++) if (thr[j].used && thr[j].detached && !thr[j].fn) wup_thread_join(j, 100);   /* detached ones that have ended */
    while (i < NTHR && thr[i].used) i++;
    if (i == NTHR || !cur) return -100;
    if (pri < 1) pri = 1;
    size = (size + 7) & ~7u; if (size < 2048) size = 2048;
    thr[i].stack = malloc(size); thr[i].done = wup_sem_new(0, 1);
    if (!thr[i].stack || thr[i].done <= 0) { free(thr[i].stack); if (thr[i].done > 0) wup_sem_free(thr[i].done); return -101; }
    thr[i].fn = fn; thr[i].arg = arg; thr[i].size = size; thr[i].used = 1; thr[i].detached = 0;
    if (evt > 0) CLR_FLG(evt, ~(1u << (i + 1)));     /* (a wake-up left over for one killed before it) */
    u32 tt = R32(KER + 0x50);
    for (int cand = 1; cand <= (int)R16(tt) && !id; cand++) {       /* a task of ours from before, at rest and not in use here */
        u32 t = tcb(cand); int busy = 0;
        if (!t || *(volatile u8 *)(t + 0x12) != 0x10 || !app_ram(R32(t + 0x3c))) continue;
        for (int j = 0; j < NTHR; j++) if (j != i && thr[j].used && thr[j].id == cand) busy = 1;
        if (busy) continue;
        R32(t + 0x38) = (u32)entry; R32(t + 0x3c) = (u32)thr[i].stack; R32(t + 0x40) = size; R16(t + 0x30) = (u16)pri;
        thr[i].id = id = cand;
        if (ACT_TSK(cand) < 0) id = 0;
    }
    if (!id) {
        static const char name[] = "WUP_THREAD";
        t_ctsk pk = { 2, 0, entry, (u32)pri, size, thr[i].stack, name };
        id = ACRE_TSK(&pk);
        if (id > 0) thr[i].id = id;
    }
    if (id <= 0) { free(thr[i].stack); wup_sem_free(thr[i].done); thr[i].used = 0; return id ? id : -102; }   /* (the kernel's error code) */
    return i;
}
int wup_thread_join(int t, int ms) {
    if (t < 0 || t >= NTHR || !thr[t].used) return -1;
    if (wup_sem_wait(thr[t].done, ms)) return -2;
    u32 c = tcb(thr[t].id);                        /* it has said so; now until the kernel has put it to rest */
    for (int i = 0; c && *(volatile u8 *)(c + 0x12) != 0x10 && i < 100; i++) DLY_TSK(1);
    if (c && *(volatile u8 *)(c + 0x12) == 0x10) DEL_TSK(thr[t].id);
    free(thr[t].stack); wup_sem_free(thr[t].done);
    thr[t].used = 0;
    return 0;
}
void wup_thread_kill(int t) {                      /* stop it where it is (what it holds stays held: for threads that never return) */
    if (t < 0 || t >= NTHR || !thr[t].used || t == slot_here()) return;
    unsigned st = wup_irq_off();
    for (int s = 1; s < NSEM; s++) for (int i = 0; i < sems[s].n; i++) if (sems[s].q[i] == t + 1) { for (sems[s].n--; i < sems[s].n; i++) sems[s].q[i] = sems[s].q[i + 1]; }
    wup_irq_restore(st);
    TER_TSK(thr[t].id); DEL_TSK(thr[t].id);
    free(thr[t].stack); wup_sem_free(thr[t].done);
    thr[t].used = 0;
}
void wup_thread_detach(int t) { if (t >= 0 && t < NTHR && thr[t].used) thr[t].detached = 1; }
unsigned wup__task(void) { return R32(KER + 0x34); }     /* (pthread.c) which kernel task runs: a firmware task calling into us is not "main" */
void wup__kernel_exit(void) {                     /* (libwup, when main() has returned) */
    for (int i = 0; i < NTHR; i++) if (thr[i].used && wup_thread_join(i, 1000)) wup_thread_kill(i);
    if (evt > 0) { DEL_FLG(evt); evt = 0; }
}

#endif
