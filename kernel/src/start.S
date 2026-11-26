@ The streamer's entry (linked at 0x10000), two ways in, MMU and caches off either way:
@  - the Homebrew Menu's loader (wup-homebrew, X held at power-on) copies the slot here, with Nintendo's firmware
@    stopped and the MMU and caches off, and jumps with r0 = HOOK_ARG;
@  - UART boot: the boot ROM's loader puts a 64 KB preamble (a branch here, then zeros, maybe an install request at
@    0x100: tools/uartboot.py) and this image in RAM from 0 and resets into 0.
@ The vectors are copied down to 0.

    .syntax unified
    .arm
    .section .vectors, "ax"
    .global _start
_start:
    ldr     pc, v_reset
    ldr     pc, v_und
    ldr     pc, v_swi
    ldr     pc, v_pabt
    ldr     pc, v_dabt
    nop
    ldr     pc, v_irq
    ldr     pc, v_fiq
v_reset: .word reset
v_und:  .word und_entry
v_swi:  .word swi_entry
v_pabt: .word pabt_entry
v_dabt: .word dabt_entry
v_irq:  .word irq_entry
v_fiq:  .word fiq_entry

    .text
reset:
    mov     r11, r0                     @ main's argument: HOOK_ARG from the loader (os.h), anything else under UART boot
    msr     cpsr_c, #0xd3               @ SVC, IRQ and FIQ off
    mov     r0, #0
    mcr     p15, 0, r0, c7, c7, 0       @ invalidate I and D caches
    mcr     p15, 0, r0, c8, c7, 0       @ and the TLBs
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #0x2000             @ vectors at 0
    bic     r0, r0, #0x1000             @ I-cache off until the MMU is set up (mmu.c)
    bic     r0, r0, #0x0005             @ MMU, D-cache off
    mcr     p15, 0, r0, c1, c0, 0

    ldr     r0, =_start                 @ linked above 0 (UART boot): the vector page goes to 0, where the CPU takes
    cmp     r0, #0                      @ exceptions (its branches are pc-relative, its targets absolute)
    beq     1f
    mov     r1, #0
    add     r2, r0, #0x40
2:  ldr     r3, [r0], #4
    str     r3, [r1], #4
    cmp     r0, r2
    blo     2b
1:
    msr     cpsr_c, #0xd2               @ IRQ
    ldr     sp, =_irq_top
    msr     cpsr_c, #0xd7               @ abort
    ldr     sp, =_abt_top
    msr     cpsr_c, #0xdb               @ undefined
    ldr     sp, =_und_top
    msr     cpsr_c, #0xd1               @ FIQ
    ldr     sp, =_abt_top
    msr     cpsr_c, #0xdf               @ system
    ldr     sp, =_svc_top
    msr     cpsr_c, #0xd3               @ SVC: the OS runs here
    ldr     sp, =_svc_top

    ldr     r0, =_bss_start             @ clear .bss (16-byte aligned, linker script)
    ldr     r1, =_bss_end
    mov     r2, #0
    mov     r3, #0
1:  cmp     r0, r1
    stmialo r0!, {r2, r3}
    blo     1b

    mov     r0, r11
    bl      main
2:  b       2b

@ IRQ: r0-r3, r12, lr saved (24 bytes: the IRQ stack stays 8-byte aligned for C). When a handler asked for a task
@ switch (sched_switch: the tick, a semaphore), the interrupted task's frame goes on its own SVC stack instead:
@ [cpsr, r0-r12, lr, pc] (sched.c), and the next task's frame is taken back (ctx_restore).
irq_entry:
    sub     lr, lr, #4
    push    {r0-r3, r12, lr}
    bl      irq_dispatch
    ldr     r0, =sched_switch
    ldr     r1, [r0]
    cmp     r1, #0
    ldmeq   sp!, {r0-r3, r12, pc}^      @ (no switch: back to the task)
    mov     r1, #0
    str     r1, [r0]
    ldm     sp!, {r0-r3, r12, lr}       @ the task's registers again; lr: where it resumes
    push    {r0}
    ldr     r0, =irq_scratch            @ the resume address and the task's cpsr, kept across the mode change
    str     lr, [r0]
    mrs     lr, spsr
    str     lr, [r0, #4]
    pop     {r0}
    msr     cpsr_c, #0xd3               @ SVC (the task's mode and stack), interrupts off
    sub     sp, sp, #4                  @ (the pc's place)
    push    {r0-r12, lr}
    ldr     r0, =irq_scratch
    ldr     r1, [r0]
    str     r1, [sp, #56]
    ldr     r1, [r0, #4]
    push    {r1}                        @ the frame: [cpsr, r0-r12, lr, pc]
    mov     r0, sp
    bl      sched_pick                  @ -> the next task's sp
    mov     sp, r0
    b       ctx_restore

@ sched_yield_asm(): (interrupts off, SVC) this task's frame on its stack, resuming at the caller; the next task's back
    .global sched_yield_asm
sched_yield_asm:
    sub     sp, sp, #4
    bic     r12, lr, #1                 @ (a Thumb caller: bit 0 of lr, the T bit of the frame's cpsr)
    str     r12, [sp]
    push    {r0-r12, lr}
    mrs     r0, cpsr
    tst     lr, #1
    orrne   r0, r0, #0x20
    push    {r0}
    mov     r0, sp
    bl      sched_pick
    mov     sp, r0
@ ctx_restore: sp -> a frame [cpsr, r0-r12, lr, pc]: the task carries on from it
ctx_restore:
    pop     {r0}
    msr     spsr_cxsf, r0
    ldm     sp!, {r0-r12, lr, pc}^
    .global ctx_restore_first
ctx_restore_first:
    mov     sp, r0
    b       ctx_restore

    .data
irq_scratch: .word 0, 0
    .text

@ relaunch_code(src, dst, words, arg): copied to high RAM and run from there (devload.c): the image over the running
@ kernel, then into it (MMU and caches already off)
    .global relaunch_code, relaunch_end
relaunch_code:
1:  ldr     r12, [r0], #4
    str     r12, [r1], #4
    subs    r2, r2, #1
    bne     1b
    mov     r0, r3
    ldr     pc, 2f
2:  .word   0x10000
relaunch_end:

@ faults: r0-r12, the faulting pc and the cpsr to fault() (exc.c), which shows them and stops
.macro FAULT kind, adj
    sub     lr, lr, #\adj
    push    {r0-r12, lr}
    mov     r0, #\kind
    mov     r1, sp
    mrs     r2, spsr
    bl      fault
3:  b       3b
.endm
und_entry:  FAULT 0, 4
swi_entry:  FAULT 1, 4
pabt_entry: FAULT 2, 4
dabt_entry: FAULT 3, 8
fiq_entry:  FAULT 4, 4
    .ltorg

