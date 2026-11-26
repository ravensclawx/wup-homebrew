#!/usr/bin/env python3
"""wup-homebrew in the GamePad's flash: what an install writes, and a copy of a stock flash image with it (the emulator).

    python3 tools/mkflash.py STOCK.bin OUT.bin [--kernel KERNEL.bin] [--menu MENU.wup] [/apps/x/x.wup=local.wup ...]

  Product1 LVC_ + 0x40       the start-up's first instruction (mov r0, #0xdf) -> b early_entry
  Product1 LVC_ + 0x50408    the early hook, near part (boot/early.c), over 764 bytes of unused NFC debug strings
  Product1 LVC_ + 0x50c9c    the early hook, far part (its A / Autoboot path), over 688 bytes of them
  0x90000 / 0x91000          the kernel (../kernel/build/kernel.bin): header {"GPST", size, crc32} / the image
  0xB0000 / 0xB1000          the menu's settings (menu/config.c; the early hook reads Autoboot there)
  0xB2000 / 0xB3000          the menu (build/apps/homebrew_menu.wup): header {"WMNU", stored size, crc32, inflated size,
                             stamp} / the stored image
  littlefs                   optional files (tools/fsimage), e.g. apps, in the spare language copy (libwup/src/fs.c)

At power-on the early hook runs before anything else of Nintendo's firmware: with A held or Autoboot on, and a whole
kernel and menu in flash, it starts the kernel; otherwise stock starts untouched. Nintendo's firmware is changed in
those three places only; the rest of ours is in the gap 0x10000-0xFFFFF, which stock does not use. Only firmware 7677
(NR4_12_20140805_release, Nintendo's last). A pad that had v1 (the hook at LVC_ + 0x63a0e) gets Nintendo's instruction
back there. A real pad gets this through tools/flash/gamepad_flash.py install-cfw (UART) or tools/install.py (over
Wi-Fi, from a pad running the menu)."""
import os, struct, subprocess, sys, time, zlib
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths

LVC_SIZE = 0xbc29c                                            # firmware 7677's LVC_ section
BANK1, SELECTOR = 0x100000, 0xf000
EARLY_SITE, EARLY_INSN = 0x40, bytes.fromhex('df00a0e3')      # (LVC offsets)
EARLY_ADDR, EARLY_MAX = 0x50408, 0x50704 - 0x50408
EARLY2_ADDR, EARLY2_MAX = 0x50c9c, 0x50f4c - 0x50c9c
EARLY_DEAD, EARLY2_DEAD = b'NFC_APP_TASK: Format Complete EVT', b'NFCAPP_Shutdown: twai_sem() Timed out!'
V1_SITE, V1_GET_BOOT_MODE, V1_STUB = 0x63a0e, 0x30734, 0xb951c   # (v1's hook: undone)
SYS_HDR, SYS_IMG, SYS_END = 0x90000, 0x91000, 0xb0000         # the kernel's slot (boot/early.c)
KMENU_HDR, KMENU_END = 0xb2000, 0x100000                      # the menu's slot (kernel/src/app.c)
KERNEL = os.path.join(os.path.dirname(paths.WUPMENU), 'kernel', 'build', 'kernel.bin')
MENU = os.path.join(paths.WUPMENU, 'build', 'apps', 'homebrew_menu.wup')


def thumb_bl(at, target):
    off = target - (at + 4)
    assert -(1 << 22) <= off < (1 << 22) and off % 2 == 0
    return struct.pack('<HH', 0xf000 | ((off >> 12) & 0x7ff), 0xf800 | ((off >> 1) & 0x7ff))


def arm_b(at, to):
    return struct.pack('<I', 0xea000000 | ((to - at - 8) >> 2 & 0xffffff))


def build_early():
    """boot/early.c -> (near part for EARLY_ADDR, far part for EARLY2_ADDR)"""
    out = paths.local_out('boot')
    o, elf, ld = (os.path.join(out, 'early' + x) for x in ('.o', '.elf', '.ld'))
    subprocess.run(['clang', '-target', 'armv5te-none-eabi', '-mthumb', '-Oz', '-ffreestanding', '-fno-builtin', '-nostdlib',
                    '-fno-jump-tables', '-c', os.path.join(paths.BOOT, 'early.c'), '-o', o], check=True)
    open(ld, 'w').write('ENTRY(early_entry) SECTIONS { . = 0x%x; .near : { *(.text.entry) *(.text) '
                        '*(.rodata*) *(.data*) *(.bss*) *(COMMON) } . = 0x%x; .far : { *(.text.far) } '
                        '/DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) } }\n' % (EARLY_ADDR, EARLY2_ADDR))
    subprocess.run([paths.LD, '-T', ld, '-o', elf, o], check=True)
    parts = []
    for sec, mx in (('.near', EARLY_MAX), ('.far', EARLY2_MAX)):
        b = os.path.join(out, 'early' + sec)
        subprocess.run([paths.OBJCOPY, '-O', 'binary', '--only-section=' + sec, elf, b], check=True)
        d = open(b, 'rb').read()
        assert len(d) <= mx, 'early hook %s too big (%d bytes, %d fit)' % (sec, len(d), mx)
        parts.append(d)
    return parts


def kernel_slot(img):
    """the kernel's slot (header sector + image) for kernel/build/kernel.bin"""
    if len(img) > SYS_END - SYS_IMG or img[:4] != bytes.fromhex('18f09fe5'): sys.exit('not a kernel image (linked at 0x10000)')
    return struct.pack('<4I', 0x54535047, len(img), zlib.crc32(img) & 0xffffffff, 0) + b'\xff' * (0x1000 - 16) + img


def menu_slot(wup):
    """the menu's slot (header sector + the stored image) from homebrew_menu.wup: its code as stored (raw deflate when
    code_raw, its inflated size, is set: libwup include/wupfmt.h)"""
    magic, hsz, off, size = struct.unpack_from('<4I', wup, 0)
    raw = struct.unpack_from('<I', wup, 0xd0)[0] if hsz >= 0xd8 else 0
    if magic != 0x30505557 or KMENU_HDR + 0x1000 + size > KMENU_END: sys.exit('not a .wup, or too big for the menu\'s slot')
    code = wup[off:off + size]
    stamp = int(os.environ.get('SOURCE_DATE_EPOCH') or time.time())
    hdr = struct.pack('<4sIIII', b'WMNU', size, zlib.crc32(code) & 0xffffffff, raw, stamp)
    return hdr + b'\xff' * (0x1000 - len(hdr)) + code


def patches(read, kernel=None, menu=None):
    """The install. read(addr, n) gives the flash as it is. -> [(flash address, bytes)] in the order to write them:
    the menu and the kernel (each header first erased, last written), the early hook's code, v1's hook undone, and last
    the branch to the early hook: until that one the pad boots exactly as before."""
    indx = read(BANK1, 0x70)
    off, size, name, _ = struct.unpack_from('<II4sI', indx, 32)
    if indx[8:12] != b'INDX' or name != b'LVC_' or size != LVC_SIZE: sys.exit('Product1 does not hold firmware 7677')
    lvc = BANK1 + off
    cur = read(lvc + EARLY_SITE, 4)
    if cur == EARLY_INSN:
        if read(lvc + EARLY_ADDR, len(EARLY_DEAD)) != EARLY_DEAD or read(lvc + EARLY2_ADDR, len(EARLY2_DEAD)) != EARLY2_DEAD:
            sys.exit('the early hook areas do not hold the expected unused strings')
    elif cur != arm_b(EARLY_SITE, EARLY_ADDR): sys.exit('unexpected start-up code at LVC 0x%x' % EARLY_SITE)
    v1 = read(lvc + V1_SITE, 4)
    if v1 not in (thumb_bl(V1_SITE, V1_GET_BOOT_MODE), thumb_bl(V1_SITE, V1_STUB)): sys.exit('unexpected code at LVC 0x%x' % V1_SITE)
    kslot = kernel_slot(kernel if kernel is not None else open(KERNEL, 'rb').read())
    mslot = menu_slot(menu if menu is not None else open(MENU, 'rb').read())
    near, far = build_early()
    blank = b'\xff' * 16
    return [(KMENU_HDR, blank), (KMENU_HDR + 0x1000, mslot[0x1000:]), (KMENU_HDR, mslot[:0x1000]),
            (SYS_HDR, blank), (SYS_IMG, kslot[0x1000:]), (SYS_HDR, kslot[:0x1000]),
            (lvc + EARLY2_ADDR, far.ljust(EARLY2_MAX, b'\0')), (lvc + EARLY_ADDR, near.ljust(EARLY_MAX, b'\0')),
            (lvc + V1_SITE, thumb_bl(V1_SITE, V1_GET_BOOT_MODE)),
            (lvc + EARLY_SITE, arm_b(EARLY_SITE, EARLY_ADDR))]


def add_files(f, files):
    import deps; deps.fetch('littlefs')
    tool = os.path.join(paths.local_out('host'), 'fsimage')
    subprocess.run([paths.host_cc(), '-O2', '-w', '-I', os.path.join(paths.THIRD, 'littlefs'), '-o', tool, os.path.join(HERE, 'fsimage.c'),
                    os.path.join(paths.THIRD, 'littlefs', 'lfs.c'), os.path.join(paths.THIRD, 'littlefs', 'lfs_util.c')], check=True)
    tmp = os.path.join(paths.local_out('host'), 'flash.tmp')
    open(tmp, 'wb').write(f)
    subprocess.run([tool, tmp, '--format'] + ['%s=%s' % pl for pl in files], check=True)
    f = bytearray(open(tmp, 'rb').read()); os.remove(tmp)
    return f


def build_image(stock, files=(), kernel=None, menu=None):
    """stock flash bytes -> the same with wup-homebrew installed. files: [(pad path, local path)] into the filesystem"""
    f = bytearray(stock)
    if f[SELECTOR] == 1: sys.exit('this dump boots Product2: expected a stock pad (Product1)')
    for a, d in patches(lambda x, n: bytes(f[x:x + n]), kernel, menu): f[a:a + len(d)] = d
    return bytes(add_files(f, files) if files else f)


def main():
    a = sys.argv[1:]
    if len(a) < 2: sys.exit(__doc__)
    def opt(k):
        if k not in a: return None
        i = a.index(k); v = open(a[i + 1], 'rb').read(); del a[i:i + 2]; return v
    kernel, menu = opt('--kernel'), opt('--menu')
    files = [tuple(x.split('=', 1)) for x in a[2:]]
    stock = open(a[0], 'rb').read()
    f = build_image(stock, files, kernel, menu)
    open(a[1], 'wb').write(f)
    changed = [s for s in range(0, len(f), 0x1000) if f[s:s + 0x1000] != stock[s:s + 0x1000]]
    print('%s: %d sectors changed%s' % (a[1], len(changed), ', %d files' % len(files) if files else ''))


if __name__ == '__main__':
    main()
