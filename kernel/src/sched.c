/* Tasks and semaphores: what libwup and lwIP need of a kernel (they used Nintendo's µITRON kernel before).
 *
 * Preemptive, by priority (a lower number is more urgent, as in µITRON); tasks of the same priority take turns every
 * SLICE ms. Everything runs in SVC mode, each task on its own stack. A task that is switched out leaves a frame on
 * its stack: [cpsr, r0-r12, lr, pc] (start.S: ctx_restore takes it back); the timer interrupt makes one when it
 * preempts (irq_entry), sched_block when a task waits or sleeps.
 *
 * Semaphores are counting, with a FIFO of waiters and a timeout. A task's end wakes whoever joins it. */
#include "os.h"

#define NTASK  48
#define NSEM   256
#define SLICE  5
enum { T_FREE, T_READY, T_SLEEP, T_WAIT, T_DEAD };
typedef struct {
    u32 sp;                        /* (saved: start.S reads it at offset 0) */
    int state, prio, sem, woke_by;
    u32 wake;                      /* ticks_ms to wake at (T_SLEEP, or T_WAIT with a timeout); 0 for ever */
    u8 *stack; u32 size; int owns_stack;
    int done;                      /* a semaphore signalled once when it ends */
    const char *name;
} task_t;
static task_t task[NTASK];
static int cur = -1, slice;
volatile int sched_switch;         /* set from interrupts: switch on the way out (start.S) */
static struct { short count, max; u8 used; } sem[NSEM];
static u32 wait_seq[NTASK], seq;             /* (when each began to wait: FIFO wake-ups) */

int sched_current(void) { return cur; }

/* the next task to run: the most urgent ready one; among equals the one after the current (round robin) */
static int pick(void) {
    int best = -1;
    for (int k = 1; k <= NTASK; k++) {
        int i = (cur + k) % NTASK;
        if (task[i].state == T_READY && (best < 0 || task[i].prio < task[best].prio)) best = i;
    }
    return best;
}
/* (start.S, interrupts off) the switch: the old task's sp kept, the new one's returned */
u32 sched_pick(u32 old_sp) {
    if (cur >= 0) task[cur].sp = old_sp;
    int n = pick();
    if (n >= 0) cur = n;
    slice = 0;
    return task[cur].sp;
}

/* (the 1 kHz tick, in the interrupt) sleepers and timeouts woken; a switch asked for when one is more urgent, or the
   current one's turn is over and another of its priority is ready */
void sched_tick(void) {
    if (cur < 0) return;
    int urgent = 0;
    for (int i = 0; i < NTASK; i++) {
        task_t *t = &task[i];
        if ((t->state == T_SLEEP || (t->state == T_WAIT && t->wake)) && (int)(ticks_ms - t->wake) >= 0) {
            t->state = T_READY; t->woke_by = -1;
            if (t->prio < task[cur].prio) urgent = 1;
        }
    }
    if (++slice >= SLICE) {
        slice = 0;
        for (int i = 0; i < NTASK; i++) if (i != cur && task[i].state == T_READY && task[i].prio <= task[cur].prio) { urgent = 1; break; }
    }
    if (task[cur].state != T_READY) urgent = 1;
    if (urgent) sched_switch = 1;
}

/* (interrupts off) give the CPU up: back when this task is ready again and picked */
void sched_yield_asm(void);                     /* start.S */
static void block(void) { sched_yield_asm(); }

static u32 irq_save(void) { u32 s; __asm__ volatile("mrs %0, cpsr\n orr r1, %0, #0xc0\n msr cpsr_c, r1" : "=&r"(s) :: "r1", "memory"); return s; }
static void irq_restore(u32 s) { __asm__ volatile("msr cpsr_c, %0" :: "r"(s) : "memory"); }

void sched_sleep(int ms) {
    if (ms <= 0) { sched_yield(); return; }
    u32 s = irq_save();
    task[cur].state = T_SLEEP; task[cur].wake = ticks_ms + (u32)ms; if (!task[cur].wake) task[cur].wake = 1;
    block();
    irq_restore(s);
}
void sched_yield(void) { u32 s = irq_save(); block(); irq_restore(s); }

/* ---- semaphores */
int sem_new(int count, int max) {
    u32 s = irq_save();
    int i = 1;
    while (i < NSEM && sem[i].used) i++;
    if (i < NSEM) { sem[i].used = 1; sem[i].count = (short)count; sem[i].max = (short)(max > 30000 ? 30000 : max < 1 ? 1 : max); }
    irq_restore(s);
    return i < NSEM ? i : -1;
}
void sem_free(int id) {
    if (id <= 0 || id >= NSEM) return;
    u32 s = irq_save();
    sem[id].used = 0;
    for (int i = 0; i < NTASK; i++) if (task[i].state == T_WAIT && task[i].sem == id) { task[i].state = T_READY; task[i].woke_by = -2; }
    irq_restore(s);
}
/* 0, or -1: timed out (ms 0: do not wait; < 0: for ever) */
int sem_wait(int id, int ms) {
    if (id <= 0 || id >= NSEM || !sem[id].used) return -1;
    u32 s = irq_save();
    if (sem[id].count > 0) { sem[id].count--; irq_restore(s); return 0; }
    if (!ms || cur < 0) { irq_restore(s); return -1; }
    task_t *t = &task[cur];
    t->state = T_WAIT; t->sem = id; t->woke_by = 0; wait_seq[cur] = ++seq;
    t->wake = ms > 0 ? ticks_ms + (u32)ms : 0; if (ms > 0 && !t->wake) t->wake = 1;
    block();
    int r = t->woke_by == 1 ? 0 : -1;
    irq_restore(s);
    return r;
}
/* the longest waiter (FIFO: the waiters are woken in the order they came, by their sequence) */
void sem_signal(int id) {
    if (id <= 0 || id >= NSEM || !sem[id].used) return;
    u32 s = irq_save();
    int w = -1;
    for (int i = 0; i < NTASK; i++)
        if (task[i].state == T_WAIT && task[i].sem == id && (w < 0 || (int)(wait_seq[i] - wait_seq[w]) < 0)) w = i;
    if (w >= 0) {
        task[w].state = T_READY; task[w].woke_by = 1; task[w].wake = 0;
        if (cur >= 0 && task[w].prio < task[cur].prio) sched_switch = 1;
    } else if (sem[id].count < sem[id].max) sem[id].count++;
    irq_restore(s);
    if (sched_switch && !(s & 0x80)) { sched_switch = 0; sched_yield(); }   /* (outside interrupts: switch now) */
}

/* ---- tasks */
static void task_end(void) {                     /* (a task's function returned) */
    u32 s = irq_save();
    task[cur].state = T_DEAD;
    irq_restore(s);
    sem_signal(task[cur].done);
    s = irq_save();
    for (;;) block();
}
/* a task running fn(arg) on the given stack (NULL: one from the kernel's heap), at priority prio. Its id, or < 0 */
int task_start(void (*fn)(void *), void *arg, void *stack, u32 size, int prio, const char *name) {
    u32 s = irq_save();
    int i = 0;
    while (i < NTASK && task[i].state != T_FREE) i++;
    if (i == NTASK) { irq_restore(s); return -1; }
    task[i].state = T_DEAD;                      /* (taken: not runnable yet) */
    irq_restore(s);
    task_t *t = &task[i];
    size = (size + 7) & ~7u; if (size < 1024) size = 1024;
    t->owns_stack = !stack;
    t->stack = stack ? stack : kmalloc(size);
    if (!t->stack) { t->state = T_FREE; return -2; }
    t->size = size; t->prio = prio; t->name = name; t->done = sem_new(0, 1);
    u32 *f = (u32 *)(((u32)t->stack + size) & ~7u) - 16;           /* the first frame: [cpsr, r0-r12, lr, pc] */
    for (int k = 0; k < 16; k++) f[k] = 0;
    f[0] = 0x13 | ((u32)fn & 1 ? 0x20 : 0);                          /* SVC, interrupts on; Thumb if fn is */
    f[1] = (u32)arg;
    f[14] = (u32)task_end;
    f[15] = (u32)fn & ~1u;
    t->sp = (u32)f;
    s = irq_save();
    wait_seq[i] = ++seq;
    t->state = T_READY;
    irq_restore(s);
    return i;
}
void task_exit(void) { task_end(); }
/* wait for task id to end, then its slot freed: 0, or -1 (timed out) */
int task_join(int id, int ms) {
    if (id < 0 || id >= NTASK || task[id].state == T_FREE) return -1;
    if (task[id].state != T_DEAD && sem_wait(task[id].done, ms)) return -1;
    task_reap(id);
    return 0;
}
/* a dead task's slot back (its stack too, if it was the kernel's) */
void task_reap(int id) {
    if (id < 0 || id >= NTASK || task[id].state != T_DEAD || id == cur) return;
    if (task[id].owns_stack) kfree(task[id].stack);
    sem_free(task[id].done);
    task[id].state = T_FREE;
}
/* stop another task where it is (its stack is the caller's to free); it can then be joined */
void task_kill(int id) {
    if (id < 0 || id >= NTASK || id == cur || task[id].state == T_FREE) return;
    u32 s = irq_save();
    if (task[id].state != T_DEAD) { task[id].state = T_DEAD; irq_restore(s); sem_signal(task[id].done); return; }
    irq_restore(s);
}
int task_prio(int id) { return id >= 0 && id < NTASK ? task[id].prio : -1; }

/* the first switch: from the boot stack to the most urgent task (never returns) */
void ctx_restore_first(u32 sp);                  /* start.S */
void sched_start(void) {
    irq_off();
    cur = pick();
    ctx_restore_first(task[cur].sp);
}

/* (the "tasks" shell command, logs) */
void sched_dump(void) {
    static const char *st[] = { "free", "ready", "sleep", "wait", "dead" };
    for (int i = 0; i < NTASK; i++) if (task[i].state != T_FREE)
        klog("task %d %s prio %d %s%s\n", i, task[i].name ? task[i].name : "?", task[i].prio, st[task[i].state], i == cur ? " (running)" : "");
}
