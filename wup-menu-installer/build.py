#!/usr/bin/env python3
"""Build the Homebrew Menu Installer: the payload from wup-menu (its tools/mkinstaller.py: the early hook, the kernel and
the Homebrew Menu, all from that checkout), then the app with devkitPro's wut in Docker (Dockerfile, the image DRXUtil
builds with) -> homebrew_menu_installer.wuhb

    python3 build.py                     (wup-menu: $WUPMENU, else ../wup-menu; it builds with ../libwup)

Needs Docker (on macOS: colima start). The image is x86-64: on Apple silicon it runs emulated, and the emulated
compiler crashes now and then: make is run again, it goes on where it stopped."""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
WUPMENU = os.environ.get('WUPMENU') or os.path.join(os.path.dirname(ROOT), 'wup-menu')
IMAGE = 'wup-menu-installer-build'


def payload(out):
    """the payload from wup-menu (tools/mkinstaller.py)"""
    tool = os.path.join(WUPMENU, 'tools', 'mkinstaller.py')
    if not os.path.isfile(tool): sys.exit('wup-menu not found at %s (set WUPMENU)' % WUPMENU)
    subprocess.run([sys.executable, tool, out], check=True)


def main():
    os.makedirs(os.path.join(ROOT, 'payload'), exist_ok=True)
    payload(os.path.join(ROOT, 'payload', 'payload.bin'))
    plat = ['--platform', 'linux/amd64']
    subprocess.run(['docker', 'build', '-q'] + plat + ['-t', IMAGE, ROOT], check=True, stdout=subprocess.DEVNULL)
    for attempt in range(4):
        if subprocess.run(['docker', 'run', '--rm'] + plat + ['-v', ROOT + ':/project', IMAGE, 'make']).returncode == 0: break
    else: sys.exit('the build failed')
    print(os.path.join(ROOT, 'homebrew_menu_installer.wuhb'))


if __name__ == '__main__':
    main()
