# libwup

The SDK for **Wii U GamePad** (WUP-010) homebrew: the runtime every app is built on, an app format (`.wup`) and
the tools that build, send and debug apps. Apps run on the [wup-homebrew kernel](../kernel), started from the
Homebrew Menu ([wup-menu](../wup-menu)), each with one flat block of about 1.8 MB.

Apps are plain C against familiar APIs, on standard, maintained open-source libraries:

| For | An app uses | Underneath |
|---|---|---|
| C library | `<stdio.h>`, `<stdlib.h>`, `malloc`, `fopen`... | [picolibc](https://github.com/picolibc/picolibc), LLVM compiler-rt, [littlefs](https://github.com/littlefs-project/littlefs) |
| GUI | `<lvgl.h>` | [LVGL](https://lvgl.io) 9 |
| Threads | `<pthread.h>`, `<semaphore.h>` | the wup-homebrew kernel's tasks and semaphores |
| Network | `<sys/socket.h>`, `<netdb.h>`, `<poll.h>` | [lwIP](https://savannah.nongnu.org/projects/lwip/)'s sockets |
| The pad | `wup.h`: screen (double buffered), buttons, sticks, touch, 48 kHz audio, rumble, brightness, microphone | the kernel's drivers (`include/wupk.h`) |

Only the hardware glue is written here (the drivers are the kernel's) and the host tools.

## Layout

| Folder | What |
|---|---|
| `include/` | `wup.h`, `wup_lvgl.h`, `gfx.h`, `fs.h`, the RAM map (`wup_ram.h`), the kernel's services (`wupk.h`), the hand-off block and app format (`handoff.h`, `wupfmt.h`), the POSIX headers |
| `src/` | the runtime over the kernel, the system layer, threads, sockets glue, the LVGL port |
| `config/` | LVGL's and lwIP's options |
| `fonts/` | Inter (SIL OFL 1.1) in the formats the runtime uses |
| `tools/` | build, send, log, debug; the library builds |
| `examples/` | `hello`: the smallest app |
| `tests/` | apps that check the runtime: `sdk`, `lvgl`, `exec`, `kernel`, `pthread`, `sockets` |
| `docs/` | [writing apps](docs/apps.md) |
| `third_party/` | fetched by the builds, not in git |

## Quick start

Needs LLVM (`clang`, `ld.lld`), Python 3 with Pillow, meson and ninja. macOS: `brew install llvm lld meson ninja`.

```
python3 tools/wupbuild.py examples/hello             # -> build/apps/hello_world.wup (the first build makes the C library and LVGL)
python3 tools/wuplink.py build/apps/hello_world.wup  # press Y in the Homebrew Menu first: it is kept and started
python3 tools/wuplog.py                              # its printf lines
```

An app in its own folder builds the same way, its `.wup` going to `build/apps` where the command runs:
`python3 ../libwup/tools/wupbuild.py .`

## Tools

| Tool | What |
|---|---|
| `wupbuild.py` | build an app folder into a `.wup` |
| `wuplink.py` | send a `.wup` to the Netloader (Y on the pad): it is kept and started |
| `wuplog.py`, `uartlog.py` | an app's log lines over Wi-Fi, or the UART |
| `wupmon.py`, `wupcall.py` | the Homebrew Menu's debug server (Developer Mode): memory, flash, calls |
| `sdk.py`, `lvgl.py`, `lwip.py`, `deps.py` | build and fetch the libraries (the builds call them) |
| `wupack.py`, `mkfont_*.py` | the `.wup` format, the fonts |

## Notes

- Only GamePad firmware LVC rev 7677 (`NR4_12_20140805_release`, Nintendo's last) is supported.
- No Nintendo code or firmware is included.
- Third-party code is fetched, not kept in git, each under its own licence (`tools/deps.py`). Included with
  their licences: the Inter fonts (SIL OFL 1.1) and Hello World's icon (Twemoji, CC-BY 4.0).
- Related: [kernel](../kernel), [wup-menu](../wup-menu) (the early hook and the Homebrew Menu),
  [wup-emu](../wup-emu) (the GamePad emulator).

## Licence

GPL-3.0-or-later ([LICENSE](../LICENSE)), as the rest of [wup-homebrew](..).
