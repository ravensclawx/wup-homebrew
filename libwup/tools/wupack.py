#!/usr/bin/env python3
"""Pack a GamePad app into a .wup file (code + name, author, version, description, icon), like a Switch NRO.

    python3 tools/wupack.py APP.bin --name "Title" [--author A] [--version 1.0] [--description D]
                            [--icon icon.png] [-o out.wup]

tools/wupbuild.py calls pack() itself; the command line is for an app image built some other way
(HBRW header, linked for 0x350000). The icon (any image Pillow reads) is fitted to 64x64 and stored as
RGB565, so the pad never decodes PNG. Without an icon the menu shows a placeholder (a red X).
Format: include/wupfmt.h."""
import os, struct, sys, zlib
ICON = 64
HDR_V2, HDR_V3 = 208, 216
HDR_V4 = 236                       # (+ 5 reserved words: v1 code for big memory, 0 now)


def deflate(data):
    """raw deflate (no zlib header): what the loader inflates (tinf)"""
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def rgb565(im):
    im = im.convert('RGB')
    return b''.join(struct.pack('<H', (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)) for r, g, b in im.getdata())


def make_icon(path):
    """an image -> ICON x ICON RGB565 (fitted, centred on white where it does not fill the square)"""
    from PIL import Image
    im = Image.open(path).convert('RGBA')
    im.thumbnail((ICON, ICON), Image.LANCZOS)
    bg = Image.new('RGBA', (ICON, ICON), (255, 255, 255, 255))
    bg.paste(im, ((ICON - im.width) // 2, (ICON - im.height) // 2), im)
    return rgb565(bg)


def pack(code, name, author='', version='1.0', icon=None, description=''):
    """an app image -> a .wup: the header (flags: WUP_FLAG_KERNEL, include/wupfmt.h), the code deflated, the icon"""
    if code[:4] != b'HBRW': raise ValueError('the code is not an app image (no HBRW header)')
    iconpix = make_icon(icon) if icon else b''
    raw = len(code); code = deflate(code)                 # stored deflated; the CRC is of what is stored
    code_off = HDR_V4
    icon_off = (code_off + len(code) + 3) & ~3 if iconpix else 0
    hdr = struct.pack('<IIIIIIII32s32s16s96sIIIIIII', 0x30505557, HDR_V4, code_off, len(code), zlib.crc32(code) & 0xffffffff,
                      icon_off, ICON if iconpix else 0, ICON if iconpix else 0, name.encode()[:31], author.encode()[:31],
                      version.encode()[:15], description.encode()[:95], raw, 1, 0, 0, 0, 0, 0)
    assert len(hdr) == HDR_V4
    out = hdr + code
    if iconpix: out += b'\0' * (icon_off - len(out)) + iconpix
    return out


def main():
    a = sys.argv[1:]
    if not a: sys.exit(__doc__)
    opt = lambda k, d=None: a[a.index(k) + 1] if k in a else d
    src = a[0]
    code = open(src, 'rb').read()
    name = opt('--name') or code[12:44].split(b'\0')[0].decode() or os.path.splitext(os.path.basename(src))[0]
    data = pack(code, name, opt('--author', ''), opt('--version', '1.0'), opt('--icon'), opt('--description', ''))
    out = opt('-o') or os.path.join(os.path.dirname(os.path.abspath(src)), '%s.wup' % name.replace(' ', '_').lower())
    open(out, 'wb').write(data)
    print('%s: %s, code %d bytes, %s, %d bytes total' % (out, name, len(code), 'icon %dx%d' % (ICON, ICON) if opt('--icon') else 'no icon', len(data)))


if __name__ == '__main__':
    main()
