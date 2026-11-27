# Architecture

wup-homebrew installs next to the GamePad's stock firmware: an **early hook** at the very start of Nintendo's firmware
picks, at power-on, between stock (untouched) and our own **kernel**, which runs the **Homebrew Menu** and its apps
with nothing of Nintendo's running. Only LVC rev 7677 (`NR4_12_20140805_release`, Nintendo's last) is supported:
every firmware address used was derived from that build.

| Folder | What |
|---|---|
| `boot/` | `early.c`: the early hook (in two pieces over unused text in Nintendo's firmware) |
| `menu/` | The Homebrew Menu, an app like the others: `main.c` (state, network, apps), `ui.c` (LVGL screens), `wifi/`, `ftpd.c`, `wuplink.c` (the Netloader), `dbgsrv.c` (Developer Mode) |
| `tools/` | `mkflash.py` (the install, and emulator images), `install.py` (install and update over Wi-Fi), `mkinstaller.py` (the Wii U installer's payload), `mkrelease.py` (Software Update's zip), `mkrepo.py` (an App Store repository), `flash/` (the UART install and backups) |
| `common/` | `catalog.c` (the App Store's repositories: libget's format), `release.c` (Software Update: GitHub Releases), `version.h` |

Built with [libwup](../../libwup) and the [kernel](../../kernel) next to this folder. Installing:
[install.md](install.md). The emulator: [wup-emu](../../wup-emu).

## Boot (derived from the firmware, verified on the real pad)

Nothing in the boot chain checks signatures or checksums:

- **Boot ROM** (4 KiB, in the chip) loads stage 2 from flash 0 to RAM 0x3F0000.
- **Stage 2** (Nintendo's, never changed) sets the clocks, does the power chip's handshake, picks Product1 (0x100000)
  unless the bank selector byte at flash 0xF000 is 1 (Product2, 0x500000), loads `LVC_` (INDX entry 2) to RAM 0 and
  jumps to it; LVC's reset vector branches to its start-up at 0x40.
- **The early hook.** The start-up's first instruction (`mov r0, #0xdf`, LVC 0x40) is a branch to `boot/early.c`,
  which sits in two runs of unreferenced NFC debug strings (LVC 0x50408, 764 bytes; LVC 0x50c9c, 688 bytes). Nothing
  of Nintendo's firmware has run yet. It saves the registers, reads the buttons from the power chip over SPI and, if
  A is held or the menu's Autoboot is on (its settings at 0xB0000), checks that the menu's slot holds a menu and the
  kernel's slot a whole kernel (CRC, through a buffer: nothing in RAM touched yet), then copies the kernel to RAM
  0x10000 and starts it. In every other case it puts back the bus registers it used and the flash's address mode,
  runs the instruction it replaced, and Nintendo's firmware starts exactly as it would have.
- **The kernel** ([kernel](../../kernel)) sets the pad up as the stock firmware does, and runs the menu from its
  slot. To start an app the menu stages its image, fills in the hand-off block (libwup `include/handoff.h`) and
  returns `WUP_HANDOFF_RUN` (`wup_exec`); the kernel runs it, then the menu again. Each app gets the whole app block.
- **A Wii U install** ([wup-menu-installer](../../wup-menu-installer)) can only write a firmware bank: it brings the
  kernel and the menu as a bundle at 0x2C9000 in the bank (after its last section). The early hook takes the kernel
  from a bundle in the bank that runs first, then from its slot; the kernel copies the bundle into the slots at that
  first start and removes it (`kernel/src/app.c`), and from then on the pad is as one installed over UART.
- **Firmware updates** (the pad's side, LVC 0x2b122): the console's update is written to the bank the pad is *not*
  running, then activated with the bank selector byte. A stock update to a pad with wup-homebrew writes a bank
  without the early hook: the pad then boots stock, and our parts in the gap stay until it is installed again.

## Flash (32 MiB N25Q256)

Nintendo's partitions: SPL 0x0, ErrorLog 0xE000, Bank 0xF000, Product1 0x100000, Product2 0x500000, Lang1 0x900000,
Lang2 0x1100000, TVcode 0x1900000, Screen1/2 0x1B00000 / 0x1B80000, Service 0x1C00000, diagnostics from 0x1FDE000.

| Flash | Contents |
|---|---|
| Product1 LVC_ + 0x40 (0x1000b4) | `b` to the early hook (was `mov r0, #0xdf`) |
| Product1 LVC_ + 0x50408 / 0x50c9c | the early hook, its two pieces |
| 0xB0000, 0xB1000 | the menu's settings (two `WCFG` records with sequence number and CRC; Autoboot is in them) |
| 0xB2000 / 0xB3000 | the kernel's slot (to 0xD1FFF, 128 KB): header `{"GPST", size, crc32}` / the kernel |
| 0xD2000-0xFFFFF | free (184 KB, for either to grow into) |
| 0x10000 / 0x11000 | the menu's slot (to 0xAFFFF, 636 KB; version 1 kept its loader and its menu here): header `{"WMNU", stored size, crc32, inflated size, stamp}` / menu image |
| 0x900000-0x10FFFFF or 0x1100000-0x18FFFFF | littlefs (8 MB: `/apps`, app data) in whichever of **Lang1 / Lang2** the pad does not use for its message screens (below) |

Everything of ours but the three changes in Product1 is in the gap 0x10000-0xFFFFF, which stock leaves blank.
Version 1 changed LVC_ + 0x63a0e (a `bl` to its hook); installing version 2 puts Nintendo's instruction back there.

**The filesystem's place.** The pad keeps its own message screens (pairing, errors, updating, low battery, the TV
remote: pictures, in every language) twice, like its firmware: Lang1 and Lang2. A selector in the power chip's EEPROM
(0x1256: 1 = Lang2, anything else = Lang1) says which copy Nintendo's firmware uses. A language update would be written
to the other copy and the selector flipped, so which copy is in use differs from pad to pad. The filesystem lives in the
copy **not** in use (`libwup/src/fs.c`): the one that already holds it, or, on a pad without one, the copy the selector
does not name, and only if the one it names really holds the screens; in any doubt, no filesystem. Checked in wup-emu
with every flash read logged: selector 0 reads Lang1 only, selector 1 reads Lang2 only. No language update comes to
these pads (firmware 7677 is Nintendo's last; the installers send firmware only).

## RAM

The kernel's and libwup's one map: [kernel/README.md](../../kernel/README.md#ram), `libwup/include/wup_ram.h`.

## Before touching real hardware

- **Keep a full backup** taken with 4-byte addressing (`tools/flash/gamepad_flash.py backup`), and a way to reflash
  the chip. A mistake in the early hook stops the stock firmware from booting too.
- **Never write a whole old dump back.** A dump read with 3-byte addressing has its upper 16 MiB mirrored from the
  lower half; writing it would destroy Service and everything above 16 MiB.
- **Developer Mode has no password.** Anything on your network can read and write the pad's memory and flash while
  it is on (that is how `tools/install.py` works). It is off on new installs; Settings > System > Developer Mode. File
  sharing without a password is open to your network too: set one in Settings > Network > File Sharing.
