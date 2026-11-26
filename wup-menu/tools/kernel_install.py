#!/usr/bin/env python3
"""The wup-homebrew kernel and the menu built for it, onto a pad running the Homebrew Menu, over Wi-Fi (Developer Mode).

    WUP_PORT=tcp:192.168.4.100 python3 tools/kernel_install.py [--loader]

  0x90000 / 0x91000   the kernel (../kernel/build/kernel.bin): header {"GPST", size, crc32}, the image
  0xB2000 / 0xB3000   its menu (WUP_KERNEL=1 build: build/apps-kernel/homebrew_menu.wup): header {"WMNU", ...}, the image
  --loader            also the loader (0x10000: X at power-on starts the kernel; boot/loader.c), written last and so
                      that any interruption leaves a pad that boots stock (as menu_install.py loader does)
  --early             also the early hook (boot/early.c) into Nintendo's firmware in bank 1: X at power-on then starts
                      the kernel before any of Nintendo's drivers. Its two code areas first (unused strings), the
                      branch at LVC 0x40 last: until that one 4-byte change the pad boots as before

Each header is invalidated first and written last: a half-written part is never taken for whole (no kernel: X boots
stock). Then hold X while powering on. Nothing of Nintendo's and nothing of the A menu is touched."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths
from wupmon import WupMon
from menu_install import Flasher
import mkflash

KERNEL = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'kernel', 'build', 'kernel.bin')
KMENU = os.path.join(os.path.dirname(HERE), 'build', 'apps-kernel', 'homebrew_menu.wup')


def main():
    a = sys.argv[1:]
    port = os.environ.get('WUP_PORT')
    if not port: sys.exit(__doc__)
    kslot = mkflash.system_slot(open(KERNEL, 'rb').read())
    mslot = mkflash.kernel_menu_slot(open(KMENU, 'rb').read())
    m = WupMon(port)
    print('kernel %d KB, its menu %d KB' % (len(kslot) >> 10, len(mslot) >> 10))
    f = Flasher(m, mkflash.SYS_HDR, mkflash.KMENU_END)
    f.sector(mkflash.SYS_HDR, b'\xff' * 16)
    f.region(mkflash.SYS_IMG, kslot[0x1000:], 'kernel')
    f.sector(mkflash.SYS_HDR, kslot[:0x1000])
    f.sector(mkflash.KMENU_HDR, b'\xff' * 16)
    f.region(mkflash.KMENU_HDR + 0x1000, mslot[0x1000:], 'its menu')
    f.sector(mkflash.KMENU_HDR, mslot[:0x1000])
    if m.flash(mkflash.SYS_HDR, len(kslot), progress=False) != kslot: sys.exit('the kernel reads back different')
    if m.flash(mkflash.KMENU_HDR, len(mslot), progress=False) != mslot: sys.exit('its menu reads back different')
    print('kernel and its menu written and verified')
    if '--loader' in a:
        loader, _ = mkflash.build_loader()
        loader = loader.ljust(0x2000, b'\xff')
        g = Flasher(m, mkflash.LOADER_FLASH, mkflash.LOADER_FLASH + 0x2000)
        g.sector(mkflash.LOADER_FLASH, b'\xff' * 16)                  # (no "CFW1" now: the hook passes it by, stock boots)
        g.sector(mkflash.LOADER_FLASH + 0x1000, loader[0x1000:])
        g.sector(mkflash.LOADER_FLASH, loader[:0x1000])
        if m.flash(mkflash.LOADER_FLASH, 0x2000, progress=False) != loader: sys.exit('LOADER READ-BACK DIFFERS: the pad boots stock; install over UART')
        print('loader written and verified')
    if '--early' in a:
        import struct
        rd = lambda fa, n: m.flash(fa, n, progress=False)
        lvc = mkflash.BANK1 + struct.unpack_from('<I', rd(mkflash.BANK1, 0x70), 32)[0]
        want = [lvc + x for x in (mkflash.EARLY2_ADDR, mkflash.EARLY_ADDR, mkflash.EARLY_SITE)]
        early = [p for p in mkflash.patches(rd) if p[0] in want]        # (patches() checks the firmware and the areas)
        assert [p[0] for p in early] == want, early
        g = Flasher(m, mkflash.BANK1, mkflash.BANK1 + 0x100000)
        for fa, data in early:                                         # each 4 KB sector read, changed, written, checked
            s0 = fa & ~0xfff
            n = ((fa + len(data) + 0xfff) & ~0xfff) - s0
            cur = bytearray(m.flash(s0, n, progress=False))
            new = bytearray(cur); new[fa - s0:fa - s0 + len(data)] = data
            for o in range(0, n, 0x1000):
                if new[o:o + 0x1000] != cur[o:o + 0x1000]: g.sector(s0 + o, bytes(new[o:o + 0x1000]))
            if m.flash(fa, len(data), progress=False) != data: sys.exit('EARLY HOOK READ-BACK DIFFERS at 0x%x: install over UART' % fa)
        print('early hook written and verified')
    print('Done. Power the pad off, then on holding X: the menu on the wup-homebrew kernel. A: the menu as before.')


if __name__ == '__main__':
    main()
