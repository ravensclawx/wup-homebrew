/* The kernel: one image at 0x10000, where the early hook copies it (wup-menu/boot/early.c); below the hand-off block
   and the apps' memory (KERNEL_END, ram.h).
   RAM is 4 MiB; ram.h has the rest of the map. */
ENTRY(_start)
SECTIONS {
    . = 0x10000;
    .text : { KEEP(*(.vectors)) *(.text*) }
    .rodata : { *(.rodata*) _rodata_end = .; }
    .data : { *(.data*) }
    . = ALIGN(16);
    _image_end = .;
    .bss (NOLOAD) : { _bss_start = .; *(.bss*) *(COMMON) . = ALIGN(16); _bss_end = .; }
    . = ALIGN(16384);
    _ttb = .; . += 16384;                     /* first-level page table (mmu.c) */
    . = ALIGN(8);
    . += 0x8000; _svc_top = .;
    . += 0x1000; _irq_top = .;
    . += 0x400;  _abt_top = .;
    . += 0x400;  _und_top = .;
    _heap_start = .;
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
