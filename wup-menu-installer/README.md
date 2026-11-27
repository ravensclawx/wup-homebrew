# Homebrew Menu Installer

A Wii U app that puts the Homebrew Menu on the GamePad, or takes it off again, through the console's own GamePad
firmware update. No soldering, no backup to restore: the GamePad writes the new firmware to its *other* firmware
bank and switches to it only once it is complete, so an update that fails leaves the GamePad as it was.

## Use

Needs a Wii U running [Aroma](https://aroma.foryour.cafe/) (or Tiramisu) and a GamePad with the console's last firmware
(it reports `190c0117`, revision 7677; any console on system 5.5.x has it).

1. **Had version 1?** Open the version 1 installer first and choose **Remove**, then continue here. Version 2 is a new
   design (its own kernel) and starts from a GamePad with Nintendo's firmware.
2. Put `homebrew_menu_installer.wuhb` in `sd:/wiiu/apps/` and open it from the Wii U Menu.
3. **Install**, then **A**. Keep the GamePad charging and close to the console; it takes a couple of minutes.
4. Turn the GamePad off, then on while holding **A**: the Homebrew Menu. The first start takes a few seconds longer.
   Without A it starts as always.

**Remove** puts the console's copy of the original firmware back the same way. Apps and settings on the GamePad stay
(in flash the firmware never reads) and come back if you install again.

## What it changes

The console keeps a copy of Nintendo's GamePad firmware (`drc_fw.bin`, in the GamePad firmware title on the
system memory). The installer reads it, checks that it is exactly the firmware the early hook was made for (its version
and the CRC-32 of the code), and adds (`source/fwpatch.c`, its payload from wup-menu's `tools/mkinstaller.py`):

- the early hook (wup-menu's `boot/early.c`) in two pieces over unused debug strings, and the branch to it at the very
  start of the firmware: the same three changes a UART install makes;
- the bundle after the firmware's last section: the kernel and the Homebrew Menu, as their own flash slots store them.

Everything else is Nintendo's firmware as it was, byte for byte, version included. At power-on the early hook checks
for A (or the menu's Autoboot): without it, Nintendo's firmware starts as if nothing were there. With it, the hook
starts the kernel from the bundle; on that first start the kernel copies the kernel and the menu into their own slots
in the free flash below the first firmware bank, then removes the bundle. From then on the GamePad is exactly as one
installed over UART: the menu updates itself with Software Update, and keeps its files (apps, their data) in the
GamePad's spare copy of its language data, which the firmware never reads (wup-menu: `docs/architecture.md`).

## Build

```
colima start                         # (macOS: Docker)
python3 build.py                     # -> homebrew_menu_installer.wuhb
python3 test/test_fwpatch.py ../wup-firmware/flash.bin build/emu.bin   # the patcher, on the host
../wup-emu/wup-emu --flash build/emu.bin --hold a 0 4000 ...             # a Wii U-installed pad
test/preview/run.sh                  # the screens as PNGs (build/preview)
python3 art/make.py                  # the icon and splash screens (icon.png, tv-splash.png, drc-splash.png)
```

It builds next to [wup-menu](../wup-menu) and [libwup](../libwup)
(`../wup-menu`, `../libwup`; or `$WUPMENU`): the payload (the early hook, the kernel and the Homebrew Menu) comes from
wup-menu's `tools/mkinstaller.py`, so the installer always carries the menu of that checkout. The app is built with
devkitPro's wut, libmocha and SDL2 in the same Docker image DRXUtil uses.

## Credits and licence

Built on [DRXUtil](https://github.com/GaryOderNichts/DRXUtil) by GaryOderNichts: the firmware update sequence
(`source/drc.cpp`), the drawing code (`source/Gfx.cpp`, `source/SDL_FontCache.c`), `source/ProcUI.cpp`, the fonts
in `data/` and the firmware format. Like DRXUtil, this folder is under the **GNU GPL, version 2 or later**
([LICENSE](LICENSE)). The fonts keep their own licences: Font Awesome Free (SIL OFL 1.1), Terminus (SIL OFL 1.1).
