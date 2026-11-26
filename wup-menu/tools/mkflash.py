#!/usr/bin/env python3
"""The custom firmware: what changes in the GamePad's flash, and a copy of a stock flash image with it (the emulator).

    python3 tools/mkflash.py STOCK.bin OUT.bin [--system IMAGE] [--kernel-menu MENU.wup] [/apps/x/x.wup=local.wup ...]

  Product1 LVC_ + 0x63a0e   bl get_boot_mode -> bl hook        (the firmware's boot-app selection)
  Product1 LVC_ + 0xb951c   the hook (boot/hook.S, over 160 bytes of unused WPS-CLI debug strings)
  0x10000                   the loader (boot/loader.c, runs at RAM 0x340000, "CFW1" header), up to 8 KB
  0x30000 / 0x31000         the menu slot: header {"WMNU", stored size, crc32, inflated size, stamp} / menu image
  0x90000 / 0x91000         --system: a whole system for X at power-on (boot/loader.c: the wup-homebrew kernel's
                            kernel/build/kernel.bin): header {"GPST", size, crc32} / the image (to 0xB0000)
  0xB2000 / 0xB3000         --kernel-menu: the menu built for that kernel (WUP_KERNEL=1, build/apps-kernel/
                            homebrew_menu.wup): header {"WMNU", stored size, crc32, inflated size, stamp} / image
  littlefs                  optional files (tools/fsimage), e.g. apps
Everything else of ours is in the free gap 0x10000-0xFFFFF too (libwup/src/fs.c); Nintendo's partitions are never
touched beyond those two patches. Only LVC rev 7677 (NR4_12_20140805_release). A real pad gets the same through
tools/flash/gamepad_flash.py install-cfw (UART); a pad that runs the menu updates it with tools/menu_install.py.

The Wii U install (installer/) cannot reach the gap: a firmware update writes only the bank the pad is not running,
then switches the bank selector to it. So that firmware carries the same two patches plus the bundle (build_bundle),
after its last section at BUNDLE_OFF: the loader (0x0000), the menu's header sector (0x2000) and image (0x3000).
The hook and the loader look there first, in the bank that runs."""
import os, struct, subprocess, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths

HOOK_SITE, GET_BOOT_MODE = 0x63a0e, 0x30734
STUB_ADDR, STUB_MAX = 0xb951c, 0xb95bc - 0xb951c
# the early hook (boot/early.c): LVC's start-up at 0x40 (mov r0, #0xdf) becomes a branch to it, in unreferenced NFC
# debug strings. X at power-on: the kernel from its slot, before any of Nintendo's drivers ran
EARLY_SITE, EARLY_INSN = 0x40, bytes.fromhex('df00a0e3')
EARLY_ADDR, EARLY_MAX = 0x50408, 0x50704 - 0x50408
EARLY2_ADDR, EARLY2_MAX = 0x50c9c, 0x50f4c - 0x50c9c
EARLY_DEAD, EARLY2_DEAD = b'NFC_APP_TASK: Format Complete EVT', b'NFCAPP_Shutdown: twai_sem() Timed out!'
LVC_SIZE = 0xbc29c
LOADER_RAM, LOADER_FLASH, LOADER_MAX = 0x340000, 0x10000, 0x2000   # the loader (in the free gap, libwup/src/fs.c)
MENU_HDR, MENU_IMG = 0x30000, 0x31000                        # the menu slot (boot/loader.c)
MENU_SLOT_END = 0x90000                                       # the menu slot: header sector + up to 380 KB of image (libwup/src/fs.c has the rest)
BANK1, SELECTOR = 0x100000, 0xf000
FW_DATA_SIZE = 0x2c8ea5                                       # firmware 7677: its sections end here (INDX .. IMG_)
BUNDLE_OFF = 0x2c9000                                         # the Wii U install's bundle, in the bank (boot/hook.S)
CFLAGS = ['-target', 'armv5te-none-eabi', '-mthumb', '-Oz', '-ffreestanding', '-fno-builtin', '-nostdlib',
          '-fno-zero-initialized-in-bss', '-fno-common', '-Wall', '-Wno-unused-function', '-I', paths.INCLUDE, '-I', paths.BOOT, '-I', paths.GEN]


def thumb_bl(at, target):
    off = target - (at + 4)
    assert -(1 << 22) <= off < (1 << 22) and off % 2 == 0
    return struct.pack('<HH', 0xf000 | ((off >> 12) & 0x7ff), 0xf800 | ((off >> 1) & 0x7ff))


def build_loader():
    """boot/loader.c -> image for the loader slot ("CFW1" header first), linked at 0x340000"""
    out = paths.local_out('boot')
    o, elf, binf, ld = (os.path.join(out, 'loader' + x) for x in ('.o', '.elf', '.bin', '.ld'))
    import deps, sdk; deps.fetch('tinf'); sdk.ensure()
    tinf = os.path.join(deps.DEPS['tinf'][0], 'src')
    inc = ['-isystem', os.path.join(sdk.sysroot('thumb'), 'include'), '-I', tinf, '-DNDEBUG', '-ffunction-sections']   # (the C library's headers only)
    o2 = os.path.join(out, 'tinflate.o')
    subprocess.run(['clang'] + CFLAGS + inc + ['-DBUNDLE_OFF=0x%x' % BUNDLE_OFF, '-c', os.path.join(paths.BOOT, 'loader.c'), '-o', o], check=True)
    subprocess.run(['clang'] + CFLAGS + inc + ['-w', '-c', os.path.join(tinf, 'tinflate.c'), '-o', o2], check=True)
    open(ld, 'w').write('SECTIONS { . = 0x%x; .header : { KEEP(*(.header)) } .text : { *(.text*) } .rodata : { *(.rodata*) }'
                        ' .data : { *(.data*) *(.bss*) *(COMMON) } /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) } }\n' % LOADER_RAM)
    subprocess.run([paths.LD, '--gc-sections', '--entry=0', '-u', 'header_', '-T', ld, '-o', elf, o, o2], check=True)
    subprocess.run([paths.LD, '--gc-sections', '--entry=0', '-u', 'header_', '-T', ld, '--oformat', 'binary', '-o', binf, o, o2], check=True)
    img = open(binf, 'rb').read()
    assert img[:4] == b'CFW1', 'the loader does not start with its header'
    assert len(img) <= LOADER_MAX, 'the loader is %d bytes (%d fit)' % (len(img), LOADER_MAX)
    return img, struct.unpack_from('<I', img, 4)[0]


def arm_b(at, to):
    return struct.pack('<I', 0xea000000 | ((to - at - 8) >> 2 & 0xffffff))


def build_early():
    """boot/early.c: (near part at EARLY_ADDR, far part at EARLY2_ADDR: the X path)"""
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
        subprocess.run([os.path.join(os.path.dirname(paths.LD), 'llvm-objcopy') if os.path.exists(os.path.join(os.path.dirname(paths.LD), 'llvm-objcopy')) else 'llvm-objcopy',
                        '-O', 'binary', '--only-section=' + sec, elf, b], check=True)
        d = open(b, 'rb').read()
        assert len(d) <= mx, 'early hook %s too big (%d bytes, %d fit)' % (sec, len(d), mx)
        parts.append(d)
    return parts


def build_stub():
    """boot/hook.S at STUB_ADDR"""
    out = paths.local_out('boot')
    o, elf, binf, ld = (os.path.join(out, 'hook' + x) for x in ('.o', '.elf', '.bin', '.ld'))
    defs = ['-DLAUNCHER_RAM=0x%x' % LOADER_RAM, '-DLOADER_FLASH=0x%x' % LOADER_FLASH, '-DLOADER_MAX=0x%x' % LOADER_MAX, '-DBUNDLE_OFF=0x%x' % BUNDLE_OFF]
    subprocess.run(['clang', '-target', 'armv5te-none-eabi', '-mthumb', '-c', os.path.join(paths.BOOT, 'hook.S'), '-o', o] + defs, check=True)
    open(ld, 'w').write('ENTRY(_start) SECTIONS { . = 0x%x; .text : { *(.text*) } }\n' % STUB_ADDR)
    subprocess.run([paths.LD, '-T', ld, '--oformat', 'binary', '-o', binf, o], check=True)
    stub = open(binf, 'rb').read()
    assert len(stub) <= STUB_MAX, 'hook stub too big (%d bytes, %d fit)' % (len(stub), STUB_MAX)
    return stub


MENU_DIR = os.environ.get('MENU_APP') or paths.MENU     # (MENU_APP: another app folder in the menu's place, for tests)


def build_menu():
    """the menu: an app like any other (tools/wupbuild.py), stored deflated in its slot -> (image, stored bytes)"""
    import wupbuild, wupack
    menu = wupbuild.build_app(MENU_DIR)[0][0]          # (the image; the menu has no code for WUP_BIGMEM)
    stored = wupack.deflate(menu)
    if MENU_IMG + len(stored) > MENU_SLOT_END: sys.exit('the menu (%d KB stored) does not fit its slot (%d KB)' % (len(stored) >> 10, (MENU_SLOT_END - MENU_IMG) >> 10))
    return menu, stored


def menu_header(menu, stored, stamp=None):
    """the menu's header sector: {"WMNU", stored size, crc32, inflated size, stamp}; the loader runs the newer stamp
    of the slot's and the bundle's (seconds; SOURCE_DATE_EPOCH or now)"""
    import time
    if stamp is None: stamp = int(os.environ.get('SOURCE_DATE_EPOCH') or time.time())
    return struct.pack('<4sIIII', b'WMNU', len(stored), zlib.crc32(stored) & 0xffffffff, len(menu), stamp)


BUNDLED_APPS = [os.path.join(paths.WUPMENU, 'apps', 'app_store'), os.path.join(paths.WUPMENU, 'apps', 'software_update')]


def build_apps():
    """the apps a Wii U install puts on the pad: [(id, version, .wup bytes)] (menu/bundle.c copies them on first start)"""
    import wupbuild, wupack
    out = []
    for d in BUNDLED_APPS:
        (code, name, author, version, icon, description) = wupbuild.build_app(d)
        out.append((name.replace(' ', '_').lower(), version, wupack.pack(code[0], name, author, version, icon, description, code[1])))
    return out


def build_bundle():
    """the Wii U install's bundle (BUNDLE_OFF in the bank): the loader (0x0000), the menu's header sector (0x2000) and
    image (0x3000), then the apps (menu/bundle.c): at the next 4 KB a table {"WAPS", count, then per app: id[32],
    version[16], offset from the bundle's start, size, crc32}, and the .wup files"""
    loader, _ = build_loader()
    menu, stored = build_menu()
    b = bytearray(loader.ljust(0x2000, b'\xff') + menu_header(menu, stored).ljust(0x1000, b'\xff') + stored)
    b += b'\xff' * (-len(b) % 0x1000)
    apps = build_apps()
    table = struct.pack('<4sI', b'WAPS', len(apps))
    at = len(b) + 0x1000
    files = b''
    for wid, version, data in apps:
        table += struct.pack('<32s16sIII', wid.encode(), version.encode(), at + len(files), len(data), zlib.crc32(data) & 0xffffffff)
        files += data + b'\xff' * (-len(data) % 4)
    assert len(table) <= 0x1000
    b += table.ljust(0x1000, b'\xff') + files
    assert BUNDLE_OFF + len(b) <= 0x400000, 'the bundle does not fit the bank (%d KB over)' % ((BUNDLE_OFF + len(b) - 0x400000) >> 10)
    return bytes(b)


def patches(read):
    """The install: the hook into Product1 in place, the loader and the menu into the free gap (libwup/src/fs.c).
    read(addr, n) gives the flash as it is. -> [(flash address, bytes)] in the order to write them: the menu (its
    header last), the loader, the hook stub, and last the bl that reaches it. Until that one the pad boots as before."""
    indx = read(BANK1, 0x70)
    off, size, name, _ = struct.unpack_from('<II4sI', indx, 32)
    if indx[8:12] != b'INDX' or name != b'LVC_' or size != LVC_SIZE: sys.exit('Product1 does not hold firmware 7677')
    lvc = BANK1 + off
    site, at = lvc + HOOK_SITE, lvc + STUB_ADDR
    cur = read(site, 4)
    if cur == thumb_bl(HOOK_SITE, GET_BOOT_MODE):
        dead = read(at, STUB_MAX)
        if not (dead.startswith(b'wpscli_del_wl_prov_svc_ie') and b'wpscli_set_wl_prov_svc_ie' in dead):
            sys.exit('the hook area does not hold the expected unused strings')
    elif cur != thumb_bl(HOOK_SITE, STUB_ADDR): sys.exit('unexpected boot code at LVC 0x%x' % HOOK_SITE)
    esite, eat = lvc + EARLY_SITE, lvc + EARLY_ADDR
    cur = read(esite, 4)
    if cur == EARLY_INSN:
        if read(eat, len(EARLY_DEAD)) != EARLY_DEAD or read(lvc + EARLY2_ADDR, len(EARLY2_DEAD)) != EARLY2_DEAD:
            sys.exit('the early hook areas do not hold the expected unused strings')
    elif cur != arm_b(EARLY_SITE, EARLY_ADDR): sys.exit('unexpected start-up code at LVC 0x%x' % EARLY_SITE)
    early, early2 = build_early()
    stub = build_stub()
    loader, _ = build_loader()
    menu, stored = build_menu()
    hdr = menu_header(menu, stored)
    menu = stored
    return [(MENU_IMG, menu), (MENU_HDR, hdr), (LOADER_FLASH, loader), (at, stub.ljust(STUB_MAX, b'\0')),
            (site, thumb_bl(HOOK_SITE, STUB_ADDR)), (lvc + EARLY2_ADDR, early2.ljust(EARLY2_MAX, b'\0')), (eat, early.ljust(EARLY_MAX, b'\0')), (esite, arm_b(EARLY_SITE, EARLY_ADDR))]


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


SYS_HDR, SYS_IMG, SYS_END = 0x90000, 0x91000, 0xb0000               # X at power-on: a whole system (boot/loader.c)
KMENU_HDR, KMENU_END = 0xb2000, 0x100000                            # its own menu (kernel/src/app.c)


def system_slot(img):
    """the system slot's bytes (header sector + image) for a system image, e.g. the streamer"""
    if len(img) > SYS_END - SYS_IMG or img[:4] != bytes.fromhex('18f09fe5'): sys.exit('not a system image (linked at 0x10000)')
    return struct.pack('<4I', 0x54535047, len(img), zlib.crc32(img) & 0xffffffff, 0) + b'\xff' * (0x1000 - 16) + img


def kernel_menu_slot(wup):
    """the kernel's menu slot's bytes (header sector + the stored image) from a .wup built with WUP_KERNEL=1: the
    .wup's code as it is stored (raw deflate when code_raw, its inflated size, is set: libwup include/wupfmt.h)"""
    import time
    magic, hsz, off, size = struct.unpack_from('<4I', wup, 0)
    raw = struct.unpack_from('<I', wup, 0xd0)[0] if hsz >= 0xd8 else 0
    if magic != 0x30505557 or KMENU_HDR + 0x1000 + size > KMENU_END: sys.exit('not a .wup, or too big for the kernel\'s menu slot')
    code = wup[off:off + size]
    stamp = int(os.environ.get('SOURCE_DATE_EPOCH') or time.time())
    hdr = struct.pack('<4sIIII', b'WMNU', size, zlib.crc32(code) & 0xffffffff, raw, stamp)
    return hdr + b'\xff' * (0x1000 - len(hdr)) + code


def build_image(stock, files=(), system=None, kmenu=None):
    """stock flash bytes -> CFW flash bytes. files: [(pad path, local path)] put into the pad filesystem; system: an
    image for the system slot (X at power-on)"""
    f = bytearray(stock)
    if f[SELECTOR] == 1: sys.exit('this dump boots Product2: expected a stock pad (Product1)')
    for a, d in patches(lambda x, n: bytes(f[x:x + n])): f[a:a + len(d)] = d
    if system:
        s = system_slot(system)
        f[SYS_HDR:SYS_END] = s + b'\xff' * (SYS_END - SYS_HDR - len(s))
    if kmenu:
        s = kernel_menu_slot(kmenu)
        f[KMENU_HDR:KMENU_END] = s + b'\xff' * (KMENU_END - KMENU_HDR - len(s))
    return bytes(add_files(f, files) if files else f)


def main():
    a = sys.argv[1:]
    if len(a) < 2: sys.exit(__doc__)
    system = None
    if '--system' in a: i = a.index('--system'); system = open(a[i + 1], 'rb').read(); del a[i:i + 2]
    kmenu = None
    if '--kernel-menu' in a: i = a.index('--kernel-menu'); kmenu = open(a[i + 1], 'rb').read(); del a[i:i + 2]
    files = [tuple(x.split('=', 1)) for x in a[2:]]
    stock = open(a[0], 'rb').read()
    f = build_image(stock, files, system, kmenu)
    open(a[1], 'wb').write(f)
    changed = [s for s in range(0, len(f), 0x1000) if f[s:s + 0x1000] != stock[s:s + 0x1000]]
    print('%s: %d sectors changed%s' % (a[1], len(changed), ', %d files' % len(files) if files else ''))


if __name__ == '__main__':
    main()
