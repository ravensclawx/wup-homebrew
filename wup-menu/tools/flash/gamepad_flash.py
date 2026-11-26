#!/usr/bin/env python3
"""Safe GamePad flashing over UART boot, built on wuptool (wuptool/wuptool.py + wuptool.bin).

    python3 tools/flash/gamepad_flash.py backup                 full 32 MiB backup (+ checks)
    python3 tools/flash/gamepad_flash.py install-cfw            the hook into Product1, the loader and the menu (about 3 minutes)
    python3 tools/flash/gamepad_flash.py install-cfw --system streamer.bin   also a whole system for X at power-on
    python3 tools/flash/gamepad_flash.py restore BACKUP         Product1's two hook blocks back from BACKUP: stock again
    python3 tools/flash/gamepad_flash.py restore BACKUP --block 0x160000 --block 0x1b0000
    python3 tools/flash/gamepad_flash.py compare BACKUP         read back the CFW blocks and compare (no writes)
    common options: --port /dev/cu.usbserial-XXXX  (default: auto-detect, like wuptool)

Safety rules this script enforces (see README.md):
  * wuptool's write erases whole 64 KiB blocks, so every write here is a complete, 64 KiB-aligned
    block: install-cfw reads it from the pad and changes only its own bytes; restore takes it from a backup.
  * After writing a block it reads it back and verifies.
  * A backup is rejected if its upper 16 MiB mirrors the lower 16 MiB (3-byte addressing dump),
    if it is not exactly 32 MiB, or if the Product1 INDX table is missing.
"""
import argparse, datetime, hashlib, json, os, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(HERE, 'wuptool'))
import wuptool  # noqa: E402  (payload upload + R/W protocol)

FLASH_SIZE = 32 << 20
BLOCK = 0x10000
SELECTOR_SECTOR = 0xf000


def sha(b): return hashlib.sha256(b).hexdigest()


def check_image(img, name):
    problems = []
    if len(img) != FLASH_SIZE: problems.append('size is %d, expected %d' % (len(img), FLASH_SIZE))
    elif img[:FLASH_SIZE // 2] == img[FLASH_SIZE // 2:]:
        problems.append('upper 16 MiB is a mirror of the lower 16 MiB (read with 3-byte addressing)')
    if img[0x100008:0x10000c] != b'INDX': problems.append('no INDX table at 0x100000 (Product1)')
    if img.count(0xff) == len(img) or img.count(0) == len(img): problems.append('blank image')
    for p in problems: print('  !! %s: %s' % (name, p))
    return not problems


ATTACHED = False


def connect(port):
    if ATTACHED: return connect_attached(port)
    port = port or wuptool.find_serial_port()
    if not port: sys.exit('no serial port found; pass --port')
    print('Power the GamePad on in UART-boot mode first (UART-boot pad grounded: the screen stays off),')
    print('then the payload is sent. If it times out, power-cycle the pad and run the command again.')
    t = wuptool.WUPTool(port)
    t.connect()
    return t


def read_block(t, addr, n=BLOCK): return flash_read_verified(t, addr, n)


class ReadError(Exception): pass


def _recv_until(t, marker, timeout):
    buf = t.rx_buf; t.rx_buf = b''
    end = time.time() + timeout
    while marker not in buf:
        if time.time() > end: raise ReadError('no %r from the pad' % marker)
        n = t.ser.in_waiting
        if n: buf += t.ser.read(n); end = time.time() + timeout
        else: time.sleep(0.002)
    i = buf.index(marker) + len(marker)
    return buf[i:]


def _drain(t, quiet=2.0):
    """after a broken transfer: discard bytes until the pad has been silent for `quiet` seconds"""
    t.rx_buf = b''; last = time.time()
    while time.time() - last < quiet:
        n = t.ser.in_waiting
        if n: t.ser.read(n); last = time.time()
        else: time.sleep(0.01)


def flash_read_once(t, addr, n, stall=5.0):
    """one R command with a stall timeout (wuptool's own read waits forever if a byte is lost)"""
    t.send_cmd(b'R'); t.send_u32(addr); t.send_u32(n)
    data = bytearray(_recv_until(t, b'OK\r\n', stall))
    last = time.time()
    while len(data) < n:
        k = t.ser.in_waiting
        if k: data += t.ser.read(min(k, n - len(data))); last = time.time()
        elif time.time() - last > stall: raise ReadError('stalled at %d of %d bytes' % (len(data), n))
        else: time.sleep(0.002)
    return bytes(data)


def flash_read_verified(t, addr, n, tries=6):
    """read until two consecutive reads agree"""
    prev = None
    for attempt in range(tries):
        try: cur = flash_read_once(t, addr, n)
        except ReadError as e:
            print('    0x%07x: %s, retrying' % (addr, e)); _drain(t); prev = None; continue
        if prev is not None and cur == prev: return cur
        if prev is not None: print('    0x%07x: two reads differ, reading again' % addr)
        prev = cur
    raise ReadError('0x%07x: could not get two matching reads' % addr)


def eeprom_byte(t, addr):
    """one D command per byte (the payload never waits for the UIC, so long D reads can return zeros)"""
    t.send_cmd(b'D'); t.send_u16(addr); t.send_u16(1)
    rest = _recv_until(t, b'OK\r\n', 3)
    end = time.time() + 3
    while not rest:
        if time.time() > end: raise ReadError('EEPROM 0x%04x: no data' % addr)
        rest = t.ser.read(1)
    t.rx_buf = rest[1:]; return rest[0]


def board_config(t):
    t.send_cmd(b'C'); t.ser.write(b'\x00'); t.ser.flush()
    rest = _recv_until(t, b'OK\r\n', 3)
    while not rest: rest = t.ser.read(1)
    t.rx_buf = rest[1:]; return rest[0]


def eeprom_plausible(ee, cfg):
    if len(set(ee)) <= 1: return 'every byte is 0x%02x (the UIC did not answer)' % ee[0]
    if ee[0x206] != cfg: return 'board config byte 0x1206 is 0x%02x but the C command says 0x%02x' % (ee[0x206], cfg)
    return None


def eeprom_read_verified(t, base=0x1000, n=0x800, tries=8):
    """Two matching, plausible reads; None if the UIC never answers. The UIC (the pad's power microcontroller) takes
    about a second after power-on before it answers (measured on a pad: wup-os, 2026-11-25), and through the UART
    payload some pads' reads come back all zeros for good (issue #25; the maintainer's own pad too). The EEPROM is only
    kept with the backup, never written by the install, so the backup goes on without it."""
    cfg = board_config(t)
    print('  board config (C command): 0x%02x' % cfg)
    prev = None
    for k in range(tries):
        cur = bytes(eeprom_byte(t, base + i) for i in range(n))
        bad = eeprom_plausible(cur, cfg)
        if bad:
            print('    EEPROM read implausible: %s' % bad); prev = None
            time.sleep(1); continue                       # (the UIC may still be starting)
        if cur == prev: return cur
        prev = cur
    return None


def connect_attached(port):
    """talk to a wuptool payload that is already running (no upload, no power-cycle)"""
    t = wuptool.WUPTool(port or wuptool.find_serial_port())
    import serial
    t.ser = serial.Serial(t.port, wuptool.BAUD_RATE, timeout=1)
    _drain(t, 0.3)
    t.send_cmd(b'H'); _recv_until(t, b'OK\r\n', 3); _drain(t, 0.3)
    print('payload is running')
    return t


def cmd_eeprom(a):
    t = connect_attached(a.port) if a.attached else connect(a.port)
    try:
        ee = eeprom_read_verified(t)
    finally: t.close()
    if ee is None: sys.exit('the UIC did not answer: no EEPROM read (power the pad off, then try again)')
    out = a.out
    open(out, 'wb').write(ee)
    print('saved %s  sha256 %s  (contains the pairing key: keep private)' % (out, sha(ee)))
    d = os.path.dirname(os.path.abspath(out)); info = os.path.join(d, 'info.json')
    if os.path.exists(info):
        j = json.load(open(info)); j['eeprom_sha256'] = sha(ee); json.dump(j, open(info, 'w'), indent=2)
        sums = os.path.join(d, 'SHA256SUMS')
        if os.path.exists(sums):
            lines = [l for l in open(sums) if not l.endswith('uic_eeprom.bin\n')]
            open(sums, 'w').write(''.join(lines) + '%s  uic_eeprom.bin\n' % sha(ee))
    print('EEPROM OK')


CHUNK = 1 << 20


def cmd_backup(a):
    """Everything readable over UART: the whole 32 MiB SPI flash and the 2 KiB UIC EEPROM, every byte
    read twice and compared, plus an info file with IDs and checksums. Resumable (--resume DIR)."""
    if a.resume:
        d = a.resume
    else:
        d = os.path.join(a.out, 'gamepad_%s' % datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
        os.makedirs(d)
    part = os.path.join(d, 'flash.bin.partial'); prog = os.path.join(d, 'progress.json')
    done = json.load(open(prog)) if os.path.exists(prog) else []
    if not os.path.exists(part): open(part, 'wb').write(b'\xff' * FLASH_SIZE)
    print('backup folder:', d)
    t = connect(a.port)
    info = {'date': datetime.datetime.now().isoformat(timespec='seconds')}
    try:
        def four(cmd):
            t.send_cmd(cmd); rest = _recv_until(t, b'OK\r\n', 5)
            while len(rest) < 4: rest += t.ser.read(4 - len(rest))
            t.rx_buf = rest[4:]; return rest[:4]
        info['flash_id'] = four(b'I').hex(' ').upper()
        info['hardware_id'] = '0x%08x' % int.from_bytes(four(b'H'), 'little')
        print('flash id %s, hardware id %s' % (info['flash_id'], info['hardware_id']))
        if not info['flash_id'].startswith('20 BA 19'): print('  !! unexpected flash ID (expected 20 BA 19)')

        ee_path = os.path.join(d, 'uic_eeprom.bin')
        if not os.path.exists(ee_path):
            print('UIC EEPROM 0x1000-0x17FF (read twice)...')
            ee = eeprom_read_verified(t, 0x1000, 0x800)
            if ee is None:
                print('  !! the UIC did not answer: the backup goes on without the EEPROM (the install never writes it;')
                print('     only the flash is needed to install or to restore)')
            else:
                open(ee_path, 'wb').write(ee)
                print('UIC EEPROM ok')
        else: print('UIC EEPROM ok')

        print('SPI flash: 32 x 1 MiB, each read twice (~3 min per MiB, ~100 min total)')
        t0 = time.time(); todo = [c for c in range(FLASH_SIZE // CHUNK) if c not in done]
        with open(part, 'r+b') as f:
            for k, c in enumerate(todo):
                data = flash_read_verified(t, c * CHUNK, CHUNK)
                f.seek(c * CHUNK); f.write(data); f.flush(); os.fsync(f.fileno())
                done.append(c); json.dump(done, open(prog, 'w'))
                el = time.time() - t0; left = el / (k + 1) * (len(todo) - k - 1)
                print('  MiB %2d/32 ok  (0x%07x)  %d min elapsed, ~%d min left' % (len(done), c * CHUNK, el // 60, left // 60), flush=True)
    finally:
        t.close()

    img = open(part, 'rb').read()
    os.replace(part, os.path.join(d, 'flash.bin')); os.remove(prog)
    ok = check_image(img, 'flash.bin')
    ee = open(ee_path, 'rb').read() if os.path.exists(ee_path) else None
    info['flash_sha256'] = sha(img); info['eeprom_sha256'] = sha(ee) if ee else 'not read (the UIC did not answer)'
    info['note'] = 'Contains the GamePad pairing key (EEPROM + flash). Keep private. Boot ROM not included: it is not readable once the payload runs; bootrom.bin in the repo root matches the published SHA-256.'
    json.dump(info, open(os.path.join(d, 'info.json'), 'w'), indent=2)
    open(os.path.join(d, 'SHA256SUMS'), 'w').write('%s  flash.bin\n' % info['flash_sha256'] + ('%s  uic_eeprom.bin\n' % info['eeprom_sha256'] if ee else ''))
    for n, (s0, ln) in wuptool.PARTITIONS.items():
        if n != 'all' and img[s0:s0 + ln].count(0xff) == ln: print('  (area %s is blank/erased)' % n)
    print(json.dumps(info, indent=2))
    print('BACKUP OK: %s' % d if ok else 'BACKUP HAS PROBLEMS - keep it, but do not flash from it')


def cfw_image(backup):
    """the backup with the custom firmware built in (tools/mkflash.py: hook, loader, menu)"""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import mkflash
    return mkflash.build_image(open(backup_file(backup), 'rb').read())


def changed_blocks(a_img, b_img):
    return [i for i in range(0, FLASH_SIZE, BLOCK) if a_img[i:i + BLOCK] != b_img[i:i + BLOCK]]


FF_BLOCK = b'\xff' * BLOCK


def erase_block(t, addr):
    """64 KiB erase on the pad (E command: no data over the wire), then confirm it reads back blank"""
    print('0x%07x: erasing' % addr)
    for attempt in range(3):
        t.send_cmd(b'E'); t.send_u32(addr)
        t.rx_buf = _recv_until(t, b'OK\r\n', 10)
        try: ok = flash_read_once(t, addr, BLOCK) == FF_BLOCK
        except ReadError: _drain(t); ok = False
        if ok: print('0x%07x: erased, verified blank' % addr); return
        print('0x%07x: not blank on read-back, erasing again' % addr)
    sys.exit('0x%07x: ERASE VERIFY FAILED. Do not power off; restore this block from the backup.' % addr)


def write_blocks(t, blocks, expect_now, new):
    """write each 64 KiB block of `new`; the pad must currently hold `expect_now` there"""
    for addr in blocks:
        if new[addr:addr + BLOCK].count(0xff) == BLOCK:
            erase_block(t, addr); continue
        cur = read_block(t, addr)
        if cur == new[addr:addr + BLOCK]:
            print('0x%07x: already has the new contents, skipping' % addr); continue
        if cur != expect_now[addr:addr + BLOCK]:
            sys.exit('0x%07x: the pad does not hold what the backup says (stale backup?) - stopping, nothing written here' % addr)
        print('0x%07x: writing 64 KiB' % addr)
        t.cmd_write_data(addr, new[addr:addr + BLOCK])
        back = read_block(t, addr)
        if back != new[addr:addr + BLOCK]:
            sys.exit('0x%07x: VERIFY FAILED after writing. Do not power off; run "restore" for this block.' % addr)
        print('0x%07x: verified' % addr)


def backup_file(path):
    return os.path.join(path, 'flash.bin') if os.path.isdir(path) else path


def load_backup(path):
    img = open(backup_file(path), 'rb').read()
    if not check_image(img, path): sys.exit('refusing to use this backup')
    return img


def mkflash_():
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import mkflash
    return mkflash


def hook_blocks(mk, img):
    """Product1's two blocks with the hook (the bl first: without it the stub is never reached)"""
    lvc = 0x100000 + struct.unpack_from('<I', img, 0x100000 + 32)[0]
    return [(lvc + mk.HOOK_SITE) & ~(BLOCK - 1), (lvc + mk.STUB_ADDR) & ~(BLOCK - 1)]


def cmd_install(a):
    """The install (tools/mkflash.py patches): read from the pad what it needs, change only those bytes (the rest of
    each 64 KiB block written back as it was: the filesystem shares the loader's block), write and verify each block.
    Order: the menu (its header last), the loader, the hook stub, then the bl that reaches it: until that last
    block the pad boots exactly as before. Then the bank selector back to Product1, only if it was not."""
    mk = mkflash_()
    t = connect(a.port)
    try:
        blocks = {}
        def blk(b):
            if b not in blocks: print('0x%07x: reading' % b, flush=True); blocks[b] = bytearray(read_block(t, b))
            return blocks[b]
        def read(x, n):
            b = x & ~(BLOCK - 1)
            if x + n > b + BLOCK: sys.exit('a read crosses a block')
            return bytes(blk(b)[x - b:x - b + n])
        pl = mk.patches(read)
        if getattr(a, 'system', None):                     # the system slot first (X at power-on: boot/loader.c)
            pl = [(mk.SYS_HDR, mk.system_slot(open(a.system, 'rb').read()))] + pl
        old = {}; last = {}
        for i, (x, d) in enumerate(pl):
            for o in range(0, len(d)):
                if o and (x + o) & (BLOCK - 1): continue
                b = (x + o) & ~(BLOCK - 1); blk(b); old.setdefault(b, bytes(blocks[b])); last[b] = i
        for x, d in pl:
            for o in range(len(d)):
                b = (x + o) & ~(BLOCK - 1); blocks[b][x + o - b] = d[o]
        for b in sorted(last, key=lambda b: (last[b], -b)):
            if bytes(blocks[b]) == old[b]: print('0x%07x: already installed' % b); continue
            print('0x%07x: writing' % b, flush=True)
            t.cmd_write_data(b, bytes(blocks[b]))
            if read_block(t, b) != bytes(blocks[b]): sys.exit('0x%07x: VERIFY FAILED. Keep the pad on and run install-cfw again.' % b)
            print('0x%07x: verified' % b)
        if read_block(t, SELECTOR_SECTOR, 0x1000)[0] != 0:            # it boots Product2: back to Product1 (has the hook)
            b0 = bytearray(read_block(t, 0)); b0[0xf000] = 0
            print('0x0000000: the bank selector -> Product1', flush=True)
            t.cmd_write_data(0, bytes(b0))
            if read_block(t, 0) != bytes(b0): sys.exit('0x0000000: VERIFY FAILED. Keep the pad on and write block 0 again.')
    finally: t.close()
    print('INSTALLED. Unground the UART-boot pad and power on: hold A for the menu%s; nothing held: the stock firmware.'
          % (', X for the system' if getattr(a, 'system', None) else ''))


def cmd_restore(a):
    """Product1's two hook blocks back from the backup (the bl first): the pad boots stock again. The loader and the
    menu in the free gap stay, unused. --block ADDR restores any other block."""
    orig = load_backup(a.backup)
    blocks = [int(b, 0) & ~(BLOCK - 1) for b in a.block] if a.block else hook_blocks(mkflash_(), orig)
    print('restoring blocks: %s' % ', '.join('0x%07x' % b for b in blocks))
    t = connect(a.port)
    try:
        for addr in blocks:
            print('0x%07x: writing backup contents' % addr)
            t.cmd_write_data(addr, orig[addr:addr + BLOCK])
            if read_block(t, addr) != orig[addr:addr + BLOCK]: sys.exit('0x%07x: VERIFY FAILED' % addr)
            print('0x%07x: verified' % addr)
    finally: t.close()
    print('RESTORED.')

UNUSED_LO, UNUSED_HI = 0x10000, 0x100000      # blank on a stock pad (between the bank sector and Product1)


def cmd_blank(a):
    """erase leftover data in the unused area 0x10000-0xFFFFF back to FF (stock), e.g. the old wupcfw"""
    orig = load_backup(a.backup)
    blocks = [int(b, 0) & ~(BLOCK - 1) for b in a.block] if a.block else \
             [b for b in range(UNUSED_LO, UNUSED_HI, BLOCK) if orig[b:b + BLOCK].count(0xff) != BLOCK]
    for b in blocks:
        if not (UNUSED_LO <= b < UNUSED_HI): sys.exit('0x%07x is outside the unused area 0x10000-0xFFFFF; refusing' % b)
    if not blocks: print('nothing to blank: the unused area is already empty in the backup'); return
    new = bytearray(orig)
    for b in blocks: new[b:b + BLOCK] = b'\xff' * BLOCK
    print('blanking %d blocks: %s' % (len(blocks), ', '.join('0x%07x' % b for b in blocks)))
    t = connect(a.port)
    try: write_blocks(t, blocks, orig, bytes(new))
    finally: t.close()
    print('BLANKED (undo: restore BACKUP --block ADDR for each block)')


PRODUCT2_LO, PRODUCT2_HI = 0x500000, 0x900000


def stock_image(raw):
    """raw image with leftovers removed: the unused area 0x10000-0xFFFFF and (only while the bank
    byte selects Product1) the inactive Product2 slot become FF, as on this pad's earlier dump"""
    if raw[0xF000] != 0x00: sys.exit('bank byte is 0x%02x (not Product1): refusing to touch Product2' % raw[0xF000])
    img = bytearray(raw)
    img[UNUSED_LO:UNUSED_HI] = b'\xff' * (UNUSED_HI - UNUSED_LO)
    img[PRODUCT2_LO:PRODUCT2_HI] = b'\xff' * (PRODUCT2_HI - PRODUCT2_LO)
    return bytes(img)


def cmd_make_stock(a):
    """erase the old wupcfw (0x10000) and wupstream (Product2) on the pad, then make the backup
    folder's flash.bin the stock image (the original read is kept as flash.raw.bin)"""
    if not os.path.isdir(a.backup): sys.exit('give the backup folder')
    f, rawf = os.path.join(a.backup, 'flash.bin'), os.path.join(a.backup, 'flash.raw.bin')
    raw = open(rawf if os.path.exists(rawf) else f, 'rb').read()
    if not check_image(raw, 'raw backup'): sys.exit('refusing to use this backup')
    stock = stock_image(raw)
    blocks = changed_blocks(raw, stock)
    print('erasing %d blocks: %s' % (len(blocks), ', '.join('0x%07x' % b for b in blocks)))
    if blocks:
        t = connect(a.port)
        try: write_blocks(t, blocks, raw, stock)
        finally: t.close()
    if not os.path.exists(rawf): os.replace(f, rawf)
    open(f, 'wb').write(stock)
    sums = os.path.join(a.backup, 'SHA256SUMS')
    lines = [l for l in open(sums) if not l.rstrip().endswith((' flash.bin', ' flash.raw.bin'))] if os.path.exists(sums) else []
    open(sums, 'w').write('%s  flash.bin\n%s  flash.raw.bin\n' % (sha(stock), sha(raw)) + ''.join(lines))
    info = os.path.join(a.backup, 'info.json')
    if os.path.exists(info):
        j = json.load(open(info)); j['flash_sha256'] = sha(stock); j['flash_raw_sha256'] = sha(raw)
        j['stock_note'] = 'flash.bin = stock: 0x10000-0xFFFFF and Product2 erased (old wupcfw/wupstream); flash.raw.bin = as read'
        json.dump(j, open(info, 'w'), indent=2)
    print('STOCK: pad and %s/flash.bin now match (original kept as flash.raw.bin)' % a.backup)


def cmd_compare(a):
    """what the pad's hook blocks hold: stock (the backup), the hook of this build, or something else (no writes)"""
    orig = load_backup(a.backup)
    mk = mkflash_(); new = cfw_image(a.backup)
    t = connect(a.port)
    try:
        for addr in hook_blocks(mk, orig):
            cur = read_block(t, addr)
            print('0x%07x: %s' % (addr, 'the hook (current build)' if cur == new[addr:addr + BLOCK] else 'stock (backup)'
                                  if cur == orig[addr:addr + BLOCK] else 'other contents (another hook build?)'))
    finally: t.close()

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port')
    ap.add_argument('--attached', dest='g_attached', action='store_true', help='the wuptool payload is already running (e.g. right after a backup): do not upload')
    sp = ap.add_subparsers(dest='cmd', required=True)
    b = sp.add_parser('backup'); b.add_argument('--out', default=os.path.join(os.environ.get('WUP_FIRMWARE') or os.path.join(os.path.dirname(ROOT), 'wup-firmware'), 'backups')); b.add_argument('--verify', action='store_true', help='(always on now: every MiB is read twice)'); b.add_argument('--resume', help='continue an interrupted backup in this folder')
    i = sp.add_parser('install-cfw', help='the hook into Product1, the loader and the menu into the free gap (needs no backup)')
    i.add_argument('--system', help='also a whole system image for X at power-on (e.g. Wii U GamePad Streamer\'s build/streamer.bin)')
    r = sp.add_parser('restore'); r.add_argument('backup'); r.add_argument('--block', action='append', help='64 KiB block address (repeatable); default: the two hook blocks')
    c = sp.add_parser('compare'); c.add_argument('backup')
    m = sp.add_parser('make-stock', help='erase old wupcfw + wupstream on the pad and make the backup stock'); m.add_argument('backup')
    k = sp.add_parser('blank', help='erase blocks of the free gap 0x10000-0xFFFFF (the loader, the menu, settings, apps: after restore, nothing is left)'); k.add_argument('backup'); k.add_argument('--block', action='append')
    e = sp.add_parser('eeprom', help='dump the UIC EEPROM (byte by byte, sanity-checked)'); e.add_argument('out'); e.add_argument('--attached', action='store_true', help='payload already running: do not upload')
    a = ap.parse_args()
    global ATTACHED; ATTACHED = a.g_attached
    {'backup': cmd_backup, 'install-cfw': cmd_install, 'restore': cmd_restore, 'compare': cmd_compare, 'eeprom': cmd_eeprom, 'blank': cmd_blank, 'make-stock': cmd_make_stock}[a.cmd](a)


if __name__ == '__main__':
    main()
