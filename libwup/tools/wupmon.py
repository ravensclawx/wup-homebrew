#!/usr/bin/env python3
"""Host side of the pad monitor protocol: The Homebrew Menu's debug server over Wi-Fi (WUP_PORT=tcp:IP), or a UART monitor.

Boot the pad normally holding A (UART-boot button NOT pressed), start "UART monitor", then:

    python3 tools/wupmon.py                       interactive prompt
    python3 tools/wupmon.py r32 0xbb678 4         one command (any monitor command, see below)
    python3 tools/wupmon.py save 0 0x100000 ram.bin          read memory into a file
    python3 tools/wupmon.py load 0x33e000 code.bin           write a file into memory
    python3 tools/wupmon.py fsave 0x1c00000 0x10000 f.bin    read SPI flash into a file
    python3 tools/wupmon.py call 0x30735 0x33f000            call a function (bit 0 = thumb)

Monitor commands (passed through): ping info r8 r16 r32 w8 w16 w32 rb wb fr call sleep exit.
Local commands: save ADDR LEN FILE, load ADDR FILE, fsave FLASHADDR LEN FILE, hexdump ADDR LEN,
                eeprom FILE (UIC EEPROM 0x1100..0x17ff via the firmware driver), help, quit.
Options: --port PORT (default /dev/cu.usbserial-0001), --log FILE (append the whole session).

As a library:  from wupmon import WupMon;  m = WupMon();  m.r32(0xbb678);  m.read(addr, n);  m.call(f, a, b)
"""
import os, sys, time, zlib

try:
    import serial
except ImportError:
    sys.exit('pip3 install pyserial')

DEFAULT_PORT = os.environ.get('WUP_PORT', '/dev/cu.usbserial-0001')   # or tcp:IP (the menu's debug server)
CHUNK = 0x10000          # per rb/wb/fr transfer (monitor buffer is 0x20000)


class MonError(Exception): pass


class _Tcp:
    """the Homebrew Menu's debug server (menu/dbgsrv.c, TCP 28802) behind the serial-port interface"""
    def __init__(self, host):
        import socket
        self.sock = socket.create_connection((host, 28802), timeout=10)
        self.sock.settimeout(0.05)
    in_waiting = 0
    def read(self, n):
        import socket
        try: return self.sock.recv(max(n, 65536))
        except socket.timeout: return b''
    def write(self, b): self.sock.sendall(b)
    def flush(self): pass
    def reset_input_buffer(self):
        while self.read(1): pass


class WupMon:
    def __init__(self, port=DEFAULT_PORT, log=None, timeout=5.0):
        self.tcp = port.startswith('tcp:')
        self.s = _Tcp(port[4:]) if self.tcp else serial.Serial(port, 115200, timeout=0.05)
        self.timeout = timeout
        self.buf = b''
        self.log = open(log, 'a') if log else None
        self.s.reset_input_buffer()
        self.sync()

    # ---- low level
    def _fill(self, deadline):
        d = self.s.read(max(1, self.s.in_waiting))
        if d: self.buf += d
        elif time.time() > deadline: raise MonError('no reply from the pad (is "UART monitor" running?)')

    def _line(self, timeout=None):
        deadline = time.time() + (timeout or self.timeout)
        while b'\n' not in self.buf: self._fill(deadline)
        l, _, self.buf = self.buf.partition(b'\n')
        self._last_rx = time.time()
        l = l.rstrip(b'\r').decode('latin-1')
        if self.log: self.log.write('< %s\n' % l); self.log.flush()
        return l

    def _raw(self, n, timeout=None):
        deadline = time.time() + (timeout or max(self.timeout, n / 8000 + 5))
        while len(self.buf) < n: self._fill(deadline)
        d, self.buf = self.buf[:n], self.buf[n:]
        return d

    GAP = 0.03   # small pause between commands (wupmon 1 redraws its screen after each one)

    def _send(self, line):
        if self.tcp:
            if self.log: self.log.write('> %s\n' % line); self.log.flush()
            self.s.write(line.encode() + b'\n'); return
        wait = getattr(self, '_last_rx', 0) + self.GAP - time.time()
        if wait > 0: time.sleep(wait)
        if self.log: self.log.write('> %s\n' % line); self.log.flush()
        # The idle monitor sleeps in firmware ticks and the pad's UART RX FIFO holds only ~16 bytes,
        # so a long command sent in one go loses characters: send it in pieces that each fit.
        b = line.encode() + b'\n'
        for i in range(0, len(b), 8):
            self.s.write(b[i:i + 8]); self.s.flush()
            if i + 8 < len(b): time.sleep(0.012)

    def sync(self):
        """get in step with the monitor: send an empty line + ping, skip anything until pong/OK"""
        self.s.write(b'\n'); time.sleep(0.1)
        self.s.reset_input_buffer(); self.buf = b''
        self._send('ping')
        deadline = time.time() + self.timeout
        while True:
            l = self._line(max(0.1, deadline - time.time()))
            if l == 'pong':
                if self._line() != 'OK': raise MonError('bad ping reply')
                return

    def cmd(self, line, timeout=None):
        """run a text command; returns (output lines, final OK/ERR line)"""
        self._send(line)
        out = []
        while True:
            l = self._line(timeout)
            if l.startswith('OK'): return out, l
            if l.startswith('ERR'): raise MonError('%s: %s' % (line, l))
            out.append(l)

    # ---- typed helpers
    def _rd(self, op, addr, n):
        out, _ = self.cmd('%s 0x%x %d' % (op, addr, n))
        return [int(x, 16) for l in out for x in l.split()]

    def r8(self, a, n=None): v = self._rd('r8', a, n or 1); return v if n else v[0]
    def r16(self, a, n=None): v = self._rd('r16', a, n or 1); return v if n else v[0]
    def r32(self, a, n=None): v = self._rd('r32', a, n or 1); return v if n else v[0]
    def w8(self, a, v): self.cmd('w8 0x%x 0x%x' % (a, v))
    def w16(self, a, v): self.cmd('w16 0x%x 0x%x' % (a, v))
    def w32(self, a, v): self.cmd('w32 0x%x 0x%x' % (a, v))

    def call(self, f, *args, timeout=30):
        _, l = self.cmd('call 0x%x %s' % (f, ' '.join('0x%x' % (a & 0xffffffff) for a in args)), timeout=timeout)
        r0, r1 = (int(x, 16) for x in l.split()[1:3])
        return r0, r1

    def _block(self, op, addr, n, tries=3):
        for attempt in range(tries):
            self._send('%s 0x%x %d' % (op, addr, n))
            try: l = self._line()
            except MonError:
                print('  (no reply at 0x%x, resyncing)' % addr, file=sys.stderr)
                self.sync(); continue
            if l.startswith('ERR'): raise MonError(l)
            if not l.startswith('DATA '): raise MonError('unexpected: %r' % l)
            data = self._raw(int(l.split()[1], 16))
            tail = self._line()
            if tail.startswith('OK ') and int(tail.split()[1], 16) == zlib.crc32(data) & 0xffffffff: return data
            print('  (crc mismatch at 0x%x, retrying)' % addr, file=sys.stderr)
            last = data
            time.sleep(0.5); self.sync()
        if op == 'rb' and 'last' in locals():
            # wupmon 1 checksums memory after sending it: live data (firmware variables) changes meanwhile
            print('  WARNING 0x%x: memory changed during every read (live data); keeping the last copy' % addr, file=sys.stderr)
            return last
        raise MonError('%s 0x%x: transfer failed %d times' % (op, addr, tries))

    def _bulk(self, op, addr, n, progress=True):
        out = bytearray(); t0 = time.time()
        while len(out) < n:
            k = min(CHUNK, n - len(out))
            out += self._block(op, addr + len(out), k)
            if progress and n > CHUNK:
                el = time.time() - t0
                print('\r  %d/%d KiB  %.1f KiB/s' % (len(out) >> 10, n >> 10, len(out) / 1024 / max(el, 1e-3)), end='', file=sys.stderr, flush=True)
        if progress and n > CHUNK: print(file=sys.stderr)
        return bytes(out)

    def eeprom(self):
        """UIC EEPROM 0x1100..0x17ff through the firmware's own driver: eeprom_read(handle, off, dst, len)
        at LVC 0x129fd (rev 7677; off is from 0x1100, off+len <= 0x700; handle = *(*(0x12bf8) + 4)).
        Read twice and compared."""
        lit = self.r32(0x12bf8)
        if self.r8(lit) != 1: raise MonError('EEPROM driver not ready')
        handle, dst, reads = self.r32(lit + 4), 0x370000, []
        for _ in range(2):
            r0, _ = self.call(0x129fd, handle, 0, dst, 0x700)
            if r0: raise MonError('eeprom_read returned %d' % r0)
            reads.append(self.read(dst, 0x700, progress=False))
        if reads[0] != reads[1]: raise MonError('two EEPROM reads differ')
        return reads[0]

    def read(self, addr, n, progress=True): return self._bulk('rb', addr, n, progress)
    def flash(self, addr, n, progress=True): return self._bulk('fr', addr, n, progress)

    def flash_write(self, sector, data):
        """one 4 KB flash sector (Developer Mode over Wi-Fi): erased, written, read back on the pad"""
        if not self.tcp: raise MonError('fw: only over Wi-Fi (Developer Mode)')
        if sector & 0xfff or not 0 < len(data) <= 4096: raise MonError('fw: one 4 KB-aligned sector at a time')
        self._send('fw 0x%x %d 0x%x' % (sector, len(data), zlib.crc32(data) & 0xffffffff))
        l = self._line()
        if l != 'GO': raise MonError('fw: %s' % l)
        self.s.write(data)
        l = self._line(30)
        if not l.startswith('OK'): raise MonError('fw 0x%x: %s' % (sector, l))

    def write(self, addr, data):
        if self.tcp:                                   # TCP: the data follows "GO" in one stream
            for off in range(0, len(data), CHUNK):
                part = data[off:off + CHUNK]
                self._send('wb 0x%x %d 0x%x' % (addr + off, len(part), zlib.crc32(part) & 0xffffffff))
                l = self._line()
                if l != 'GO': raise MonError('wb: %s' % l)
                self.s.write(part)
                l = self._line(30)
                if not l.startswith('OK'): raise MonError('wb: %s' % l)
            return
        for off in range(0, len(data), CHUNK):
            part = data[off:off + CHUNK]
            self._send('wb 0x%x %d 0x%x' % (addr + off, len(part), zlib.crc32(part) & 0xffffffff))
            l = self._line()
            if l != 'GO': raise MonError('wb: %s' % l)
            for i in range(0, len(part), 64):
                blk = part[i:i + 64]
                # the monitor may doze between blocks and the RX FIFO is ~16 bytes: wake it with a few
                # bytes, then send the rest while it is busy reading
                for j in range(0, len(blk), 16):          # never more than the FIFO holds
                    self.s.write(blk[j:j + 16]); self.s.flush(); time.sleep(0.004)
                if self._raw(1) != b'.': raise MonError('wb: lost sync')
            l = self._line()
            if not l.startswith('OK'): raise MonError('wb: %s' % l)


def hexdump(addr, data):
    for o in range(0, len(data), 16):
        row = data[o:o + 16]
        print('%08x  %-48s  %s' % (addr + o, ' '.join('%02x' % b for b in row), ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)))


def run(m, words):
    num = lambda s: int(s, 0)
    c = words[0]
    if c == 'save': open(words[3], 'wb').write(m.read(num(words[1]), num(words[2]))); print('saved', words[3])
    elif c == 'fsave': open(words[3], 'wb').write(m.flash(num(words[1]), num(words[2]))); print('saved', words[3])
    elif c == 'load': m.write(num(words[1]), open(words[2], 'rb').read()); print('written')
    elif c == 'hexdump': hexdump(num(words[1]), m.read(num(words[1]), num(words[2]), progress=False))
    elif c == 'eeprom': open(words[1], 'wb').write(m.eeprom()); print('saved', words[1], '(UIC EEPROM 0x1100..0x17ff: contains the pairing key, keep private)')
    elif c == 'help': print(__doc__)
    else:
        out, last = m.cmd(' '.join(words), timeout=30)
        for l in out: print(l)
        print(last)


def main():
    args = sys.argv[1:]
    port, log = DEFAULT_PORT, None
    while args and args[0] in ('--port', '--log'):
        if args[0] == '--port': port = args[1]
        else: log = args[1]
        args = args[2:]
    m = WupMon(port, log=log)
    if args: run(m, args); return
    print('connected (type help, quit)')
    try: import readline  # noqa: F401  (history / line editing)
    except ImportError: pass
    while True:
        try: l = input('pad> ').strip()
        except EOFError: break
        if not l: continue
        if l in ('quit', 'q'): break
        try: run(m, l.split())
        except (MonError, ValueError, IndexError, OSError) as e: print('error:', e)


if __name__ == '__main__':
    main()
