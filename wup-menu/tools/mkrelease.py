#!/usr/bin/env python3
"""A release's update for Software Update (Settings > System, common/release.h): build/wup-homebrew-update.zip, to
attach to a GitHub Release whose tag is the menu's version ("v2.0.0").

    python3 tools/mkrelease.py            (builds the kernel and the menu first)

The zip holds, stored (not compressed: the menu is deflated already), kernel.bin then homebrew_menu.bin: each its
flash slot as stored (a 4 KB header sector, then the image; tools/mkflash.py kernel_slot, menu_slot)."""
import os, re, subprocess, sys, zipfile
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths, mkflash


def main():
    subprocess.run(['make', '-C', os.path.join(os.path.dirname(paths.WUPMENU), 'kernel')], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, os.path.join(paths.LIBWUP, 'tools', 'wupbuild.py'), paths.MENU], check=True, cwd=paths.WUPMENU, stdout=subprocess.DEVNULL)
    version = re.search(r'MENU_VERSION="([^"]+)"', open(os.path.join(paths.MENU, 'app.cfg')).read()).group(1)
    out = os.path.join(paths.local_out(), 'wup-homebrew-update.zip')
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_STORED) as z:
        z.writestr('kernel.bin', mkflash.kernel_slot(open(mkflash.KERNEL, 'rb').read()))
        z.writestr('homebrew_menu.bin', mkflash.menu_slot(open(mkflash.MENU, 'rb').read()))
    print('%s (%d KB): attach it to a GitHub Release tagged v%s' % (out, os.path.getsize(out) >> 10, version))


if __name__ == '__main__':
    main()
