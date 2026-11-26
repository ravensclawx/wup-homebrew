#!/usr/bin/env python3
"""wup-homebrew onto a pad running the Homebrew Menu with Developer Mode on (Settings), over Wi-Fi.

    WUP_PORT=tcp:192.168.1.50 python3 tools/install.py

What tools/mkflash.py patches() lists, in its order: the menu and the kernel (each header erased first, written last),
the early hook, v1's hook undone, and last the 4-byte branch to the early hook. Each 4 KB sector is read, changed,
written, read back; sectors that already hold the new bytes are left alone (installing again only updates). An
interruption before the last step leaves a pad that boots as before; after it, without a whole kernel and menu, A
starts stock. Then power the pad off and on holding A (or with Autoboot on)."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths
from wupmon import WupMon
import mkflash


def main():
    port = os.environ.get('WUP_PORT')
    if not port: sys.exit(__doc__)
    if not os.path.isfile(mkflash.KERNEL): sys.exit('no kernel: make -C ../kernel')
    if not os.path.isfile(mkflash.MENU): sys.exit('no menu: python3 ../libwup/tools/wupbuild.py menu')
    m = WupMon(port)
    rd = lambda a, n: m.flash(a, n, progress=False)
    plan = mkflash.patches(rd)
    pad = {}                                               # sector address -> its bytes on the pad now
    written = 0
    for addr, data in plan:                                # in the plan's order
        for s in range(addr & ~0xfff, addr + len(data), 0x1000):
            if s not in pad: pad[s] = bytearray(rd(s, 0x1000))
            lo, hi = max(addr, s), min(addr + len(data), s + 0x1000)
            new = bytearray(pad[s]); new[lo - s:hi - s] = data[lo - addr:hi - addr]
            if new != pad[s]:
                m.flash_write(s, bytes(new)); pad[s] = new; written += 1
                print('\r  %d sectors written' % written, end='', flush=True)
    print()
    for s, b in sorted(pad.items()):                       # every sector touched, read back
        if rd(s, 0x1000) != bytes(b): sys.exit('READ-BACK DIFFERS at 0x%x: install again (or over UART: tools/flash/gamepad_flash.py install-cfw)' % s)
    print('Installed and verified (%d sectors). Power the pad off, then on holding A (or with Autoboot on): wup-homebrew; '
          'otherwise stock.' % written)

if __name__ == '__main__':
    main()
