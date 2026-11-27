#!/usr/bin/env python3
"""The Wii U installer's payload: what wup-menu-installer's source/fwpatch.c adds to the console's copy of the GamePad
firmware (drc_fw.bin), which the console then sends to the pad as a firmware update (into its other bank).

    python3 tools/mkinstaller.py [OUT]            -> build/installer/payload.bin (the installer's build embeds it)

The payload (little-endian): {"WUPI", 2, firmware version, LVC_ size, LVC_ CRC-32, BUNDLE_OFF, patch count, bundle
length}, then per patch {offset in LVC_, length}, the patches' bytes, and the bundle. The patches are the UART install's
in Nintendo's firmware (tools/mkflash.py): the early hook's two pieces and the branch to it. The bundle (at BUNDLE_OFF
in the bank, after the last section) is the kernel's slot (padded to KB_MENU) and the menu's slot, as stored: the early
hook starts the kernel from it, and the kernel puts both into their own slots at that first start (kernel/src/app.c),
then removes it. The firmware must be 7677 (NR4_12_20140805_release): its version and LVC_'s CRC-32 are checked on the
console before anything is written."""
import os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths, mkflash

FW_VERSION = 0x190c0117          # drc_fw.bin's image version (the VER_ section holds the same)
LVC_CRC = 0x82d87264             # CRC-32 of LVC_ in 7677 (DRXUtil checks the same before its patches)
BUNDLE_OFF = 0x2c9000            # in the bank: after IMG_, the last section (it ends at 0x2c8ea5); boot/early.c BUNDLE
KB_MENU = 0x20000                # the menu's slot in the bundle (boot/early.c, kernel/src/app.c)


def bundle(kernel=None, menu=None):
    k = mkflash.kernel_slot(kernel if kernel is not None else open(mkflash.KERNEL, 'rb').read())
    m = mkflash.menu_slot(menu if menu is not None else open(mkflash.MENU, 'rb').read())
    assert len(k) <= KB_MENU and BUNDLE_OFF + KB_MENU + len(m) <= 0x400000
    return k.ljust(KB_MENU, b'\xff') + m


def patches():
    near, far = mkflash.build_early()
    return [(mkflash.EARLY2_ADDR, far.ljust(mkflash.EARLY2_MAX, b'\0')), (mkflash.EARLY_ADDR, near.ljust(mkflash.EARLY_MAX, b'\0')),
            (mkflash.EARLY_SITE, mkflash.arm_b(mkflash.EARLY_SITE, mkflash.EARLY_ADDR))]


def build(kernel=None, menu=None):
    pl, b = patches(), bundle(kernel, menu)
    head = struct.pack('<4s7I', b'WUPI', 2, FW_VERSION, mkflash.LVC_SIZE, LVC_CRC, BUNDLE_OFF, len(pl), len(b))
    return head + b''.join(struct.pack('<II', a, len(d)) for a, d in pl) + b''.join(d for _, d in pl) + b


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(paths.local_out('installer'), 'payload.bin')
    p = build()
    open(out, 'wb').write(p)
    print('%s: %d KB' % (out, len(p) >> 10))


if __name__ == '__main__':
    main()
