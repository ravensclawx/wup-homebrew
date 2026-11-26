# Roadmap

The aim: mainstream, maintained libraries wherever one exists, only the hardware glue written here; stock untouched
beside it; installable from a Wii U.

## Version 2 (the `v2` branch)

Done, each proven on a real pad:

- **The early hook**: A or Autoboot at power-on starts our kernel before any of Nintendo's firmware runs; otherwise
  stock, untouched. Nintendo's firmware changed in three places only.
- **The kernel**: its own drivers for the screen (double buffered, vblank flips, fades; the stock panel start), sound
  and the volume slider, buttons, the Power button (a clean power-off), flash, Wi-Fi (also after Nintendo's standby),
  microphone and rumble.
- **libwup, native**: every app one flat block of about 1.8 MB; the Nintendo-firmware backend deleted.
- **Installs**: UART (`install-cfw`) and over Wi-Fi (`tools/install.py`); installing version 2 undoes version 1's hook.

Next:

1. **The camera and the video decoder** in the kernel (wup-os's drivers), for WUP Stream.
2. **Apps that use it**: WUP Boy (save states in memory, the double buffer), WUP Stream (the decoder, the camera).
3. **The release**: the version 2 installer for the Wii U (also upgrading version 1 pads), the catalogue's kernel
   builds, WUP Boy's and WUP Stream's `app.cfg` without the old layout keys.

Lessons worth keeping:

- Port the stock firmware's sequences faithfully (the panel's start, the radio's start): guessed variants failed on
  the pad where the stock order worked at once. Trace stock and ours side by side in wup-emu to find the difference.
- Nintendo's standby keeps the radio powered with its pads isolated (DEVICE_CTL 0x08): clear that bit first, and send
  no SDIO I/O reset (it wedges the radio until a power cycle).
- A panel that loses power with a picture on it shows a ghost of it at the next start: power off through the panel.
- picolibc must be built with `-fno-builtin` (else clang turns `__aeabi_memset` into a call to itself).
- lwIP needs `SO_REUSE` for FTP's passive ports. Emulator runs: always cap the output; a crash loop logs for ever.
