# wup-menu

The **Homebrew Menu** for the **Wii U GamePad** (model WUP-010), and how wup-homebrew gets onto a pad: the early hook
in the pad's own firmware, the install tools, the menu, App Store and Software Update. They run on the
[wup-homebrew kernel](../kernel).

<p align="center"><img src="docs/images/home.png" width="427" alt="The Homebrew Menu"> <img src="docs/images/settings.png" width="427" alt="Settings"></p>

- **Hold A at power-on** (or set Autoboot) for wup-homebrew. Without A the pad boots Nintendo's firmware exactly as
  before. The Power button turns it off.
- The app list, Settings, Wi-Fi setup with an on-screen keyboard, battery, signal and a clock.
- **[WUP Boy](https://github.com/ravensclawx/wup-boy)** (in the App Store): Game Boy and Game Boy Color games you copy over File Sharing, with
  save states, rewind and fast forward. No games included.
- **App Store** and **Software Update** in the box: apps from the project's catalogue, newer kernels and menus, over HTTPS.
- **Wi-Fi** on normal WPA2 networks (5 GHz), also ones that hide their name (Other…). **File sharing** over FTP for the pad's flash filesystem (8 MB).
  **Netloader**: send an app from a computer and run it.

Apps are built with [libwup](../libwup), the SDK; the menu itself is an app like the others.

## Install

- **From a Wii U** (no soldering): the [Homebrew Menu Installer](../wup-menu-installer), a
  Wii U app that installs through the GamePad's own firmware update.
- **Over UART** (soldering): `tools/flash/`, see [docs/install.md](docs/install.md).

- **Over Wi-Fi**, on a pad running the Homebrew Menu with Developer Mode on: `tools/install.py`.

After that, Software Update keeps it current. Read the safety notes in [docs/architecture.md](docs/architecture.md) first.

## Layout

| Folder | What |
|---|---|
| `boot/` | the early hook, patched into the start of Nintendo's firmware |
| `menu/` | the Homebrew Menu |
| `tools/` | the install: `mkflash.py` (what an install writes; emulator images), `install.py` (Wi-Fi), `flash/` (UART), `mkrepo.py` (the catalogue) |
| `docs/` | [architecture](docs/architecture.md), [install](docs/install.md), [wiring](docs/wiring.md), [roadmap](docs/roadmap.md) |

## Build

Needs [libwup](../libwup) and the [kernel](../kernel) next to this folder (or `LIBWUP=path`) and what they need
(LLVM, Python 3 with Pillow, meson, ninja).

```
make -C ../kernel                                               # -> ../kernel/build/kernel.bin
python3 ../libwup/tools/wupbuild.py menu                        # -> build/apps/homebrew_menu.wup
python3 tools/mkflash.py DUMP.bin build/emu.bin                 # into a copy of your pad's flash dump: try it in wup-emu
WUP_PORT=tcp:PAD_IP python3 tools/install.py                    # install or update a pad over Wi-Fi (Developer Mode on)
```

The emulator is [wup-emu](../wup-emu); dumps of your own pad stay outside every repository
(`../wup-firmware`, or `WUP_FIRMWARE=path`).

## Notes

- Only GamePad firmware LVC rev 7677 (`NR4_12_20140805_release`, Nintendo's last) is supported.
- No Nintendo code or firmware is included; the tools need dumps of your own pad.

## The catalogue

App Store and Software Update read one catalogue in the format of the Homebrew App Store's libget
(`repo.json`, `zips/<name>.zip`, `packages/<name>/icon.png`), at `CATALOG_URL` (`common/catalog.h`).
`python3 tools/mkrepo.py` builds it from the apps, the kernel and the menu into `build/repo`; publish that folder over HTTPS
(GitHub Pages, for example). `tools/repo_server.py` serves it from this computer with a test CA, for test builds.

## Licence

GPL-3.0-or-later ([LICENSE](../LICENSE)), as the rest of [wup-homebrew](..).
