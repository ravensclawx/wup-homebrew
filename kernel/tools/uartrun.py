#!/usr/bin/env python3
"""Development: a new kernel into a pad running the kernel, over the UART cable, run from RAM at once (src/devload.c):
nothing is written to flash, the pad keeps its state (the radio as it is). Then its log, for --seconds.

    python3 kernel/tools/uartrun.py [build/kernel.bin] [--port /dev/cu.usbserial-XXXX] [--seconds 20]"""
import argparse, glob, struct, sys, time, zlib
import serial

a = argparse.ArgumentParser()
a.add_argument('image', nargs='?', default='build/kernel.bin'); a.add_argument('--port'); a.add_argument('--seconds', type=float, default=20)
o = a.parse_args()
img = open(o.image, 'rb').read()
port = o.port or (glob.glob('/dev/cu.usbserial*') + glob.glob('/dev/ttyUSB*'))[0]
s = serial.Serial(port, 115200, timeout=0.05)
s.reset_input_buffer()
s.write(b'WKRN' + struct.pack('<II', len(img), zlib.crc32(img) & 0xffffffff))
for i in range(0, len(img), 256):                     # (paced: the kernel takes bytes from a 64 KB ring every millisecond)
    s.write(img[i:i + 256]); s.flush()
print('sent %d bytes; the log follows' % len(img))
end = time.time() + o.seconds
while time.time() < end:
    d = s.read(4096)
    if d: sys.stdout.write(d.decode('latin-1').replace('\r', '')); sys.stdout.flush()
