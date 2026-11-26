# The wup-homebrew kernel

What runs the GamePad under wup-homebrew. The early hook ([wup-menu/boot/early.c](../wup-menu/boot/early.c)) loads it
from flash and starts it before any of Nintendo's firmware has run; from then on everything on the pad's CPU is ours.

```
make            # -> build/kernel.bin (an image linked at 0x10000)
```

## What it does

- **Start-up** (about 0.35 s): the board, the display and the sound codec set up as the stock firmware sets them up
  (its register writes, replayed: `fw_init.c`, `fw_audio.c`; the panel's start, LCD power cycle included, in
  `lcd.c`), the flash, then its tasks.
- **Tasks**: a preemptive priority scheduler (`sched.c`, a 1 kHz tick) with counting semaphores. Its own tasks: input
  (every 4 ms: buttons, sticks, touch, the volume slider, the Power button), the radio, the apps, and an idle task.
- **Apps** (`app.c`): the Homebrew Menu from its flash slot, then whatever app it starts, one at a time, each in the
  one flat app block (below). When an app ends, the menu comes back; sound, microphone and rumble are stopped.
- **Power**: the Power button fades the screen out, switches the panel and the LCD off, then has the power chip turn
  the pad off, as the stock firmware does.

## Drivers

| | |
|---|---|
| `lcd.c` | the screen: two RGB565 buffers, shown at the vertical blank; the backlight; the gamma curve (fades) |
| `audio.c` | 48 kHz stereo out through a ring the 1 kHz tick keeps; the microphone; the volume slider |
| `wifi.c`, `sdio.c`, `radio.c` | the Broadcom BCM4319 over SDIO: its firmware from Nintendo's bank, patched on the way (WPA2), the stock driver's bring-up, frames in and out; a radio left asleep by Nintendo's standby is woken first |
| `flash.c`, `spi.c` | the 32 MB SPI flash and the power chip (UIC) on the same bus |
| `uic.c`, `input.c` | the power chip: input reports, battery, LCD power, power off |
| `i2c.c` | the bus to the panel and the sound codec |

## Services for apps

libwup reaches the kernel through one table ([libwup/include/wupk.h](../libwup/include/wupk.h)), whose address is
at 0x80. It only grows at its end; `version` says how far it goes (now 3): tasks and semaphores, time, the input
report, the UIC, the backlight, flash, Wi-Fi (ioctls, join, frames), the log, the screen, sound, the microphone and
rumble. Apps do not call it themselves: libwup's API is built on it.

## RAM

4 MiB, one map for the kernel and libwup ([libwup/include/wup_ram.h](../libwup/include/wup_ram.h)):

| Address | What |
|---|---|
| 0x000000 | the kernel: vectors, code, data, stacks, its heap |
| 0x07f000 | the hand-off block between the menu and the apps it starts |
| 0x080000 | **the app**: one flat block to 0x253000 (about 1.8 MB): its image, its .bss, then its heap |
| 0x253000 | the sound ring, the microphone ring |
| 0x26fb00 | screen 0 (RGB565, 820 KB) |
| 0x337d80 | screen 1 (820 KB); between apps also the next app's stored image, and the 8-bit screen |

## Development

- `tools/uartrun.py`: a new kernel over the UART cable into a pad running the kernel, run from RAM at once
  (`devload.c`; nothing written to flash).
- The log is UART1 at 115200 (the battery-cover header).
