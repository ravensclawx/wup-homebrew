/* Threads and semaphores for apps: the wup-homebrew kernel's tasks and semaphores (include/wupk.h). */
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

