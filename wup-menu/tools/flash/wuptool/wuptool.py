#!/usr/bin/env python3
"""
WUP Tool - Comprehensive Wii U GamePad Tool

Interactive menu or command line. Auto-detects USB UART.
"""

import sys
import os
import glob
import serial
import time

BAUD_RATE = 115200
PAYLOAD_ADDR = 0x10000

# Flash partition definitions
PARTITIONS = {
    'spl':      (0x0000000, 0x000E000),
    'errorlog': (0x000E000, 0x0001000),
    'bank':     (0x000F000, 0x0001000),
    'bootmenu': (0x0010000, 0x00F0000),  # Custom boot menu area
    'product1': (0x0100000, 0x0400000),
    'product2': (0x0500000, 0x0400000),
    'lang1':    (0x0900000, 0x0800000),
    'lang2':    (0x1100000, 0x0800000),
    'tvcode':   (0x1900000, 0x0200000),
    'screen1':  (0x1B00000, 0x0080000),
    'screen2':  (0x1B80000, 0x0080000),
    'service':  (0x1C00000, 0x03DE000),
    'inspdata': (0x1FDE000, 0x0001000),
    'seqconf':  (0x1FDF000, 0x0001000),
    'insplog1': (0x1FE0000, 0x0010000),
    'insplog2': (0x1FF0000, 0x0010000),
    'all':      (0x0000000, 0x2000000),
}

# UIC EEPROM structure (address range 0x1000-0x17FF)
EEPROM_FIELDS = {
    # WiFi settings
    'wifi_ssid':           (0x1101, 32, 'string'),
    'wifi_ssid_len':       (0x1121, 1, 'u8'),
    'wifi_auth_type':      (0x1122, 1, 'u8'),      # 0=open, 1=shared
    'wifi_auth_mode':      (0x1123, 1, 'u8'),      # 0=disabled, 1=none, 4=WPA, 0x80=WPA2
    'wifi_encryption':     (0x1124, 1, 'u8'),      # 0=none, 1=WEP, 2=TKIP, 3=AES
    'wifi_passphrase':     (0x1125, 64, 'string'),
    'wifi_passphrase_len': (0x1165, 1, 'u8'),
    'wifi_country':        (0x1166, 2, 'u16'),
    'wifi_crc':            (0x116B, 2, 'u16'),
    'wowl_key':            (0x116D, 16, 'hex'),
    'wowl_mac':            (0x117F, 6, 'mac'),
    'wowl_setting':        (0x1187, 1, 'u8'),
    'wifi_channel':        (0x118A, 1, 'u8'),

    # Firmware status
    'fw_install_status':   (0x1010, 2, 'hex'),     # A5 5A when installed
    'led_no_fw':           (0x1013, 1, 'u8'),      # 1=red, 2=amber, 0=none

    # Board info (page 1)
    'board_info1':         (0x1200, 1, 'u8'),
    'region_info1':        (0x1203, 1, 'u8'),
    'board_config1':       (0x1206, 1, 'u8'),

    # Board info (page 2)
    'board_info2':         (0x1280, 1, 'u8'),
    'region_info2':        (0x1283, 1, 'u8'),
    'board_config2':       (0x1286, 1, 'u8'),

    # User settings (page 3)
    'language':            (0x1300, 1, 'u8'),
    'opening_screen':      (0x1303, 1, 'u8'),
    'dev_mode':            (0x1306, 1, 'u8'),
    'lcd_brightness':      (0x1386, 1, 'u8'),
    'airwave_setting':     (0x1396, 1, 'u8'),

    # Calibration
    'volume_cal1':         (0x121A, 2, 'hex'),
    'accel_cal1':          (0x1313, 12, 'hex'),
    'gyro_cal1':           (0x122C, 19, 'hex'),
    'touch_cal1':          (0x1344, 16, 'hex'),

    # Service versions
    'service_ver1':        (0x1362, 4, 'hex'),
    'service_ver2':        (0x1265, 4, 'hex'),
    'service_ver3':        (0x12E5, 4, 'hex'),

    # Remote control
    'rc_data_ver':         (0x135C, 4, 'hex'),
    'rc_id1':              (0x1389, 5, 'hex'),
    'rc_id2':              (0x138E, 5, 'hex'),
}

# Lookup tables for EEPROM values
BOARD_VERSIONS = {
    0: 'DK1', 1: 'EP_DK2', 2: 'DP1', 3: 'DP2', 4: 'DK3', 5: 'DK4',
    6: 'DP3', 7: 'DK5', 8: 'DP4', 9: 'DKMP', 10: 'DP5', 11: 'MASS',
    12: 'DKMP2', 13: 'DRC_I'
}

REGIONS = {
    0: 'Japan', 1: 'North America', 2: 'Europe', 3: 'China',
    4: 'South Korea', 5: 'Taiwan', 6: 'Australia'
}

LANGUAGES = {
    0: 'English (US)', 1: 'French (US)', 2: 'Spanish (US)', 3: 'Portuguese (US)',
    4: 'English (EU)', 5: 'French (EU)', 6: 'Spanish (EU)', 7: 'Portuguese (EU)',
    8: 'Dutch', 9: 'Italian', 10: 'German', 11: 'Russian',
    12: 'Japanese', 13: 'Chinese (Simplified)', 14: 'Korean', 15: 'Chinese (Traditional)'
}

WIFI_AUTH_MODE = {0x00: 'Disabled', 0x01: 'None', 0x04: 'WPA', 0x80: 'WPA2'}
WIFI_ENCRYPTION = {0: 'None', 1: 'WEP', 2: 'TKIP', 3: 'AES'}


def find_serial_port():
    """Auto-detect USB UART port"""
    patterns = [
        '/dev/ttyUSB*',
        '/dev/ttyACM*',
        '/dev/cu.usbserial*',          # (macOS: cu, not tty, which waits for a carrier signal)
        '/dev/tty.usbserial*',
        '/dev/cu.usbmodem*',
        '/dev/tty.usbmodem*',
    ]

    ports = []
    for pattern in patterns:
        ports.extend(glob.glob(pattern))

    if not ports:
        return None

    ports.sort()

    if len(ports) == 1:
        return ports[0]

    print("\nMultiple serial ports found:")
    for i, port in enumerate(ports):
        print(f"  [{i+1}] {port}")
    print()

    try:
        choice = input("Select port [1]: ").strip()
        if not choice:
            return ports[0]
        idx = int(choice) - 1
        if 0 <= idx < len(ports):
            return ports[idx]
    except (ValueError, EOFError, KeyboardInterrupt):
        pass

    return ports[0]


def parse_int(s):
    """Parse integer from hex or decimal string"""
    s = s.strip()
    if s.startswith('0x') or s.startswith('0X'):
        return int(s, 16)
    return int(s)


class WUPTool:
    def __init__(self, port):
        self.port = port
        self.ser = None
        self.rx_buf = b''

    def connect(self):
        """Upload payload and wait for WUPTOOL prompt"""
        self.ser = serial.Serial(self.port, BAUD_RATE, timeout=1)
        print(f"Using {self.port}")

        script_dir = os.path.dirname(os.path.abspath(__file__))
        payload_path = os.path.join(script_dir, 'wuptool.bin')
        with open(payload_path, 'rb') as f:
            payload = f.read()


        time.sleep(1)
        self.ser.reset_input_buffer()

        print("Waiting for GamePad...")
        total = PAYLOAD_ADDR + len(payload) + 4
        self.ser.write(total.to_bytes(4, 'little'))
        self.ser.write(bytearray(PAYLOAD_ADDR))
        self.ser.write(payload)
        self.ser.write(PAYLOAD_ADDR.to_bytes(4, 'little'))
        self.ser.flush()

        buf = b''
        timeout = time.time() + 30
        while b'WUPTOOL' not in buf:
            if time.time() > timeout:
                raise TimeoutError("Timeout waiting for device")
            if self.ser.in_waiting:
                data = self.ser.read(self.ser.in_waiting)
                buf += data
                sys.stdout.write(data.decode('latin-1', errors='replace'))
                sys.stdout.flush()
            time.sleep(0.01)

        print("\nConnected!")
        time.sleep(0.1)
        self.ser.reset_input_buffer()

    def close(self):
        if self.ser:
            self.ser.close()

    def send_cmd(self, cmd):
        self.ser.write(cmd)
        self.ser.flush()

    def send_u16(self, val):
        self.ser.write(val.to_bytes(2, 'little'))
        self.ser.flush()

    def send_u32(self, val):
        self.ser.write(val.to_bytes(4, 'little'))
        self.ser.flush()

    def recv_u8(self):
        if self.rx_buf:
            byte = self.rx_buf[0]
            self.rx_buf = self.rx_buf[1:]
            return byte
        while True:
            if self.ser.in_waiting:
                return self.ser.read(1)[0]
            time.sleep(0.001)

    def recv_u32(self):
        val = self.recv_u8()
        val |= self.recv_u8() << 8
        val |= self.recv_u8() << 16
        val |= self.recv_u8() << 24
        return val

    def wait_for(self, marker, show_output=False):
        buf = self.rx_buf
        self.rx_buf = b''

        while marker not in buf:
            if self.ser.in_waiting:
                data = self.ser.read(self.ser.in_waiting)
                buf += data
                if show_output:
                    sys.stdout.write(data.decode('latin-1', errors='replace'))
                    sys.stdout.flush()
            time.sleep(0.01)

        marker_pos = buf.find(marker)
        marker_end = marker_pos + len(marker)
        self.rx_buf = buf[marker_end:]
        return buf[:marker_end]

    def power_off(self):
        print("Powering off GamePad...")
        self.send_cmd(b'P')
        time.sleep(0.5)
        if self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)

    # ==================== Flash Commands ====================

    def cmd_info(self):
        """Get device info"""
        self.send_cmd(b'I')
        self.wait_for(b'OK\r\n')
        id_bytes = [self.recv_u8() for _ in range(4)]
        print(f"Flash ID: {id_bytes[0]:02X} {id_bytes[1]:02X} {id_bytes[2]:02X} {id_bytes[3]:02X}")

        self.send_cmd(b'B')
        self.wait_for(b'OK\r\n')
        boot = self.recv_u8()
        print(f"Boot partition: {boot} ({'Product1' if boot == 0 else 'Product2'})")

        self.send_cmd(b'H')
        self.wait_for(b'OK\r\n')
        hwid = self.recv_u32()
        print(f"Hardware ID: 0x{hwid:08X}", end='')
        if hwid in (0x41040, 0x1040):
            print(" (UART boot)")
        else:
            print()

        # Get service mode status
        self.send_cmd(b'C')
        self.ser.write(bytes([0]))  # Read config
        self.ser.flush()
        self.wait_for(b'OK\r\n')
        cfg = self.recv_u8()
        mode = "Service" if (cfg & 0x03) != 0x01 else "Retail"
        print(f"Board config: 0x{cfg:02X} ({mode} mode)")

    def cmd_read(self, addr, length, output_file):
        """Read flash to file"""
        print(f"Reading 0x{addr:07X} - 0x{addr+length-1:07X} ({length} bytes)")

        self.send_cmd(b'R')
        self.send_u32(addr)
        self.send_u32(length)
        self.wait_for(b'OK\r\n')

        # bytes that arrived together with "OK" are already in rx_buf (wait_for reads ahead);
        # dropping them made the read wait forever for data it had already received
        data = bytearray(self.rx_buf)
        self.rx_buf = b''
        last_pct = -1
        start_time = time.time()

        while len(data) < length:
            if self.ser.in_waiting:
                chunk = self.ser.read(min(self.ser.in_waiting, length - len(data)))
                data.extend(chunk)

            pct = len(data) * 100 // length
            if pct != last_pct and pct % 5 == 0:
                elapsed = time.time() - start_time
                if elapsed > 0:
                    rate = len(data) / elapsed / 1024
                    print(f"\r  {pct}% ({len(data)//1024}KB) - {rate:.1f} KB/s", end='', flush=True)
                last_pct = pct
            time.sleep(0.001)

        elapsed = time.time() - start_time
        rate = length / elapsed / 1024 if elapsed > 0 else 0
        print(f"\r  100% ({length//1024}KB) - {rate:.1f} KB/s")

        with open(output_file, 'wb') as f:
            f.write(data)
        print(f"Saved to {output_file}")

    def cmd_write(self, addr, input_file):
        """Write file to flash"""
        with open(input_file, 'rb') as f:
            data = f.read()

        length = len(data)
        print(f"Writing {input_file} ({length} bytes)")
        print(f"Target: 0x{addr:07X} - 0x{addr+length-1:07X}")

        self.send_cmd(b'W')
        self.send_u32(addr)
        self.send_u32(length)

        self.wait_for(b'ERASE ')
        self.wait_for(b'GO\r\n')

        print("Sending data...")
        sent = 0
        last_pct = -1
        start_time = time.time()

        while sent < length:
            chunk = data[sent:sent+64]
            self.ser.write(chunk)
            sent += len(chunk)

            if sent % 4096 == 0:
                self.ser.flush()
                time.sleep(0.001)

            pct = sent * 100 // length
            if pct != last_pct and pct % 5 == 0:
                elapsed = time.time() - start_time
                if elapsed > 0:
                    rate = sent / elapsed / 1024
                    print(f"\r  {pct}% - {rate:.1f} KB/s", end='', flush=True)
                last_pct = pct

        self.ser.flush()
        elapsed = time.time() - start_time
        rate = length / elapsed / 1024 if elapsed > 0 else 0
        print(f"\r  100% - {rate:.1f} KB/s")

        self.wait_for(b'DONE\r\n')
        print("Write complete!")

    def cmd_read_data(self, addr, length):
        """Read flash to memory (returns bytearray)"""
        self.send_cmd(b'R')
        self.send_u32(addr)
        self.send_u32(length)
        self.wait_for(b'OK\r\n')

        # Start with any bytes already in rx_buf from wait_for
        data = bytearray(self.rx_buf)
        self.rx_buf = b''

        last_pct = -1
        start_time = time.time()

        while len(data) < length:
            if self.ser.in_waiting:
                chunk = self.ser.read(min(self.ser.in_waiting, length - len(data)))
                data.extend(chunk)

            pct = len(data) * 100 // length
            if pct != last_pct and pct % 10 == 0:
                elapsed = time.time() - start_time
                if elapsed > 0:
                    rate = len(data) / elapsed / 1024
                    print(f"\r  Reading: {pct}% ({len(data)//1024}KB) - {rate:.1f} KB/s", end='', flush=True)
                last_pct = pct
            time.sleep(0.001)

        elapsed = time.time() - start_time
        rate = length / elapsed / 1024 if elapsed > 0 else 0
        print(f"\r  Reading: 100% ({length//1024}KB) - {rate:.1f} KB/s")
        return data

    def cmd_write_data(self, addr, data):
        """Write data from memory to flash"""
        length = len(data)

        self.send_cmd(b'W')
        self.send_u32(addr)
        self.send_u32(length)

        self.wait_for(b'ERASE ')
        self.wait_for(b'GO\r\n')

        sent = 0
        last_pct = -1
        start_time = time.time()

        while sent < length:
            chunk = data[sent:sent+64]
            self.ser.write(chunk)
            sent += len(chunk)

            if sent % 4096 == 0:
                self.ser.flush()
                time.sleep(0.001)

            pct = sent * 100 // length
            if pct != last_pct and pct % 10 == 0:
                elapsed = time.time() - start_time
                if elapsed > 0:
                    rate = sent / elapsed / 1024
                    print(f"\r  Writing: {pct}% - {rate:.1f} KB/s", end='', flush=True)
                last_pct = pct

        self.ser.flush()
        elapsed = time.time() - start_time
        rate = length / elapsed / 1024 if elapsed > 0 else 0
        print(f"\r  Writing: 100% - {rate:.1f} KB/s")

        self.wait_for(b'DONE\r\n')

    def cmd_patch(self, patch_file, create_restore=True):
        """Apply patches from a patch file (read-modify-write)

        Patch file format (one patch per line):
            address:find_hex:replace_hex

        Example:
            0x5E5B22:A4C0E1:000FAC
            0x5E731F:A4C0E1:000FAC

        Lines starting with # are comments.
        All patches in the same 64KB sector are batched together.

        If create_restore=True, creates a _restore.patch file with the
        original values that can be used to undo the patches.
        """
        # Parse patch file
        patches = []
        with open(patch_file, 'r') as f:
            for line_num, line in enumerate(f, 1):
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                try:
                    parts = line.split(':')
                    if len(parts) != 3:
                        print(f"Line {line_num}: Invalid format (need addr:find:replace)")
                        continue
                    addr = parse_int(parts[0])
                    find_bytes = bytes.fromhex(parts[1])
                    replace_bytes = bytes.fromhex(parts[2])
                    if len(find_bytes) != len(replace_bytes):
                        print(f"Line {line_num}: find/replace length mismatch")
                        continue
                    patches.append((addr, find_bytes, replace_bytes))
                except Exception as e:
                    print(f"Line {line_num}: Parse error: {e}")
                    continue

        if not patches:
            print("No valid patches found!")
            return False

        print(f"Loaded {len(patches)} patches from {patch_file}")

        # Group patches by 64KB sector
        SECTOR_SIZE = 0x10000
        sectors = {}
        for addr, find_bytes, replace_bytes in patches:
            sector_base = (addr // SECTOR_SIZE) * SECTOR_SIZE
            if sector_base not in sectors:
                sectors[sector_base] = []
            sectors[sector_base].append((addr, find_bytes, replace_bytes))

        print(f"Patches span {len(sectors)} sector(s)")

        # Track restore info for each patch
        restore_patches = []

        # Process each sector
        total_applied = 0
        for sector_base in sorted(sectors.keys()):
            sector_patches = sectors[sector_base]
            print(f"\n--- Sector 0x{sector_base:07X} ({len(sector_patches)} patches) ---")

            # Read sector
            print(f"Reading sector...")
            data = self.cmd_read_data(sector_base, SECTOR_SIZE)

            # Apply patches
            applied = 0
            for addr, find_bytes, replace_bytes in sector_patches:
                offset = addr - sector_base
                current = bytes(data[offset:offset + len(find_bytes)])

                if current == find_bytes:
                    # Save original value for restore patch
                    restore_patches.append((addr, replace_bytes, current))
                    data[offset:offset + len(find_bytes)] = replace_bytes
                    print(f"  [OK] 0x{addr:07X}: {find_bytes.hex().upper()} -> {replace_bytes.hex().upper()}")
                    applied += 1
                elif current == replace_bytes:
                    print(f"  [SKIP] 0x{addr:07X}: already {replace_bytes.hex().upper()}")
                    applied += 1
                else:
                    # Save what we actually found - useful for restore even on mismatch
                    restore_patches.append((addr, replace_bytes, current))
                    print(f"  [WARN] 0x{addr:07X}: expected {find_bytes.hex().upper()}, found {current.hex().upper()}")

            if applied > 0:
                # Write sector back
                print(f"Writing sector...")
                self.cmd_write_data(sector_base, data)
                print(f"Sector done! ({applied}/{len(sector_patches)} patches)")
                total_applied += applied
            else:
                print(f"No patches applied to this sector, skipping write")

        # Create restore patch file
        if create_restore and restore_patches:
            # Generate restore filename
            base_name = patch_file
            if base_name.endswith('.patch'):
                restore_file = base_name[:-6] + '_restore.patch'
            else:
                restore_file = base_name + '_restore.patch'

            with open(restore_file, 'w') as f:
                f.write(f"# Restore patch for {os.path.basename(patch_file)}\n")
                f.write(f"# Generated automatically - apply this to undo the patches\n")
                f.write(f"# Created: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
                f.write(f"#\n")
                f.write(f"# Usage: python3 wuptool.py patch {restore_file}\n\n")

                for addr, find_bytes, replace_bytes in restore_patches:
                    f.write(f"0x{addr:07X}:{find_bytes.hex().upper()}:{replace_bytes.hex().upper()}\n")

            print(f"\n=== Restore patch saved to: {restore_file} ===")

        print(f"\n=== Total: {total_applied}/{len(patches)} patches applied ===")
        return total_applied == len(patches)

    def cmd_boot(self, partition=None):
        """Get or set boot partition"""
        if partition is None:
            self.send_cmd(b'B')
            self.wait_for(b'OK\r\n')
            boot = self.recv_u8()
            print(f"Boot partition: {boot} ({'Product1' if boot == 0 else 'Product2'})")
        else:
            print(f"Setting boot partition to {partition}...")
            self.send_cmd(b'S')
            self.ser.write(bytes([partition]))
            self.ser.flush()
            resp = self.wait_for(b'\r\n')
            if b'OK' in resp:
                print("Done!")
            else:
                print("ERROR")

    def cmd_dump(self, output_dir):
        """Dump all partitions"""
        os.makedirs(output_dir, exist_ok=True)

        for name, (addr, size) in PARTITIONS.items():
            if name == 'all':
                continue
            output_file = os.path.join(output_dir, f"{name}.bin")
            print(f"\n=== {name} ===")
            self.cmd_read(addr, size, output_file)

        print(f"\nAll partitions dumped to {output_dir}")

    # ==================== Config Commands ====================

    def cmd_service_mode(self, enable=None):
        """Get or set service mode"""
        if enable is None:
            self.send_cmd(b'C')
            self.ser.write(bytes([0]))
            self.ser.flush()
            self.wait_for(b'OK\r\n')
            cfg = self.recv_u8()
            mode = "Service" if (cfg & 0x03) != 0x01 else "Retail"
            print(f"Board config: 0x{cfg:02X} ({mode} mode)")
        elif enable:
            print("Enabling service mode...")
            self.send_cmd(b'C')
            self.ser.write(bytes([1]))
            self.ser.flush()
            self.wait_for(b'OK\r\n')
            cfg = self.recv_u8()
            if (cfg & 0x03) != 0x01:
                print(f"OK! Config now 0x{cfg:02X}")
                print("Reboot WITHOUT UART to enter service mode.")
            else:
                print("FAILED")
        else:
            print("Restoring retail mode...")
            self.send_cmd(b'C')
            self.ser.write(bytes([2]))
            self.ser.flush()
            self.wait_for(b'OK\r\n')
            cfg = self.recv_u8()
            if cfg == 0x15:
                print(f"OK! Config now 0x{cfg:02X}")
            else:
                print(f"Verify: 0x{cfg:02X}")

    # ==================== Memory Commands ====================

    def cmd_mem_read(self, addr, length):
        """Read memory (RAM/registers)"""
        self.send_cmd(b'M')
        self.send_u32(addr)
        self.send_u32(length)
        self.wait_for(b'OK\r\n')

        data = bytearray()
        while len(data) < length:
            if self.ser.in_waiting:
                chunk = self.ser.read(min(self.ser.in_waiting, length - len(data)))
                data.extend(chunk)
            time.sleep(0.001)

        return bytes(data)

    def cmd_mem_write(self, addr, data):
        """Write memory (RAM/registers)"""
        self.send_cmd(b'N')
        self.send_u32(addr)
        self.send_u32(len(data))
        self.ser.write(data)
        self.ser.flush()
        self.wait_for(b'OK\r\n')

    def cmd_mem_dump(self, addr, length):
        """Hexdump memory"""
        data = self.cmd_mem_read(addr, length)
        print()
        for offset in range(0, len(data), 16):
            hex_part = ' '.join(f'{b:02X}' for b in data[offset:offset+16])
            ascii_part = ''.join(
                chr(b) if 0x20 <= b < 0x7f else '.'
                for b in data[offset:offset+16]
            )
            print(f"  {addr+offset:08X}  {hex_part:<48}  {ascii_part}")

    def cmd_bootrom_dump(self, output_file):
        """Dump bootrom (4KB at 0x0)"""
        print("Dumping bootrom (4KB at 0x0)...")
        data = self.cmd_mem_read(0, 0x1000)
        with open(output_file, 'wb') as f:
            f.write(data)
        print(f"Saved to {output_file}")

    # ==================== EEPROM Commands ====================

    def cmd_eeprom_read_chunk(self, addr, length):
        """Read a single chunk from UIC EEPROM (max ~64 bytes recommended)"""
        self.send_cmd(b'D')
        self.send_u16(addr)
        self.send_u16(length)

        # Wait for OK with timeout
        self.wait_for(b'OK\r\n')

        # Start with any bytes already buffered from wait_for
        data = bytearray(self.rx_buf)
        self.rx_buf = b''

        # EEPROM reads are slow - each byte requires UIC I2C transaction
        # Use blocking read with longer timeout
        old_timeout = self.ser.timeout
        self.ser.timeout = 0.1  # 100ms per byte max

        while len(data) < length:
            byte = self.ser.read(1)
            if not byte:
                break
            data.extend(byte)

        self.ser.timeout = old_timeout
        return bytes(data)

    def cmd_eeprom_read(self, addr, length):
        """Read from UIC EEPROM (chunked for reliability)"""
        CHUNK_SIZE = 64  # Read in 64-byte chunks for reliability

        data = bytearray()
        remaining = length
        current_addr = addr

        while remaining > 0:
            chunk_len = min(remaining, CHUNK_SIZE)
            chunk = self.cmd_eeprom_read_chunk(current_addr, chunk_len)

            if len(chunk) == 0:
                break

            data.extend(chunk)
            current_addr += len(chunk)
            remaining -= len(chunk)

            if len(chunk) < chunk_len:
                break

        return bytes(data)

    def cmd_eeprom_write(self, addr, data):
        """Write to UIC EEPROM (with unlock and commit)"""
        self.send_cmd(b'F')
        self.send_u16(addr)
        self.send_u16(len(data))
        self.ser.write(data)
        self.ser.flush()
        self.wait_for(b'OK\r\n')

    def cmd_eeprom_dump(self, output_file):
        """Dump entire EEPROM (0x1000-0x1800)"""
        print("Dumping EEPROM (0x1000-0x1800, 2KB)...")
        data = self.cmd_eeprom_read(0x1000, 0x800)
        with open(output_file, 'wb') as f:
            f.write(data)
        print(f"Saved to {output_file}")

    def cmd_eeprom_hexdump(self, addr, length):
        """Hexdump EEPROM region"""
        data = self.cmd_eeprom_read(addr, length)
        print()
        for offset in range(0, len(data), 16):
            hex_part = ' '.join(f'{b:02X}' for b in data[offset:offset+16])
            ascii_part = ''.join(
                chr(b) if 0x20 <= b < 0x7f else '.'
                for b in data[offset:offset+16]
            )
            print(f"  {addr+offset:04X}  {hex_part:<48}  {ascii_part}")

    def cmd_wifi_info(self):
        """Display WiFi configuration from EEPROM"""
        # Read WiFi data in separate chunks for reliability
        # SSID area: 0x1101-0x1124 (36 bytes)
        ssid_data = self.cmd_eeprom_read(0x1101, 36)
        # PSK area: 0x1125-0x1168 (68 bytes)
        psk_data = self.cmd_eeprom_read(0x1125, 68)
        # Channel and WoWL: 0x116D-0x118B (30 bytes)
        misc_data = self.cmd_eeprom_read(0x116D, 30)

        ssid = ""
        psk = ""
        channel = 0
        auth_mode = 0
        encryption = 0

        print("\n" + "=" * 50)
        print("              WiFi Configuration")
        print("=" * 50)

        if len(ssid_data) >= 33:
            ssid_len = ssid_data[0x20]
            ssid = ssid_data[0:min(ssid_len, 32)].decode('ascii', errors='replace')
            auth_type = ssid_data[0x21] if len(ssid_data) > 0x21 else 0
            auth_mode = ssid_data[0x22] if len(ssid_data) > 0x22 else 0
            encryption = ssid_data[0x23] if len(ssid_data) > 0x23 else 0

            print(f"\n  SSID:         {ssid}")
            print(f"  Auth:         {WIFI_AUTH_MODE.get(auth_mode, 'Unknown')} + {WIFI_ENCRYPTION.get(encryption, 'Unknown')}")

        if len(psk_data) >= 65:
            psk_len = psk_data[0x40]
            psk = psk_data[0:min(psk_len, 64)].decode('ascii', errors='replace')
            print(f"\n  PSK (hex):    {psk}")

        if len(misc_data) >= 30:
            wowl_key = misc_data[0:16]
            wowl_mac = misc_data[0x12:0x18]
            channel = misc_data[0x1D]

            print(f"\n  Channel:      {channel} (5GHz)")
            print(f"\n  WoWL Key:     {wowl_key.hex().upper()}")
            print(f"  WoWL MAC:     {':'.join(f'{b:02X}' for b in wowl_mac)}")

        print("\n" + "-" * 50)
        print("  For Wireshark decryption, use PSK above")
        print(f"  Monitor on 5GHz channel {channel}")
        print("-" * 50)

        return {
            'ssid': ssid,
            'psk': psk,
            'channel': channel,
            'auth_mode': auth_mode,
            'encryption': encryption,
        }

    def cmd_board_info(self):
        """Display board information from EEPROM"""
        print("\n--- Board Information ---\n")

        # Read board info areas
        data1 = self.cmd_eeprom_read(0x1200, 0x10)
        data2 = self.cmd_eeprom_read(0x1280, 0x10)
        data3 = self.cmd_eeprom_read(0x1300, 0x10)

        board_info = data1[0]
        region_info = data1[3]
        board_config = data1[6]

        board_main = board_info & 0x0F
        board_sub = (board_info >> 4) & 0x0F

        print(f"  Board Info:   0x{board_info:02X}")
        print(f"    Main Ver:   {board_main} ({BOARD_VERSIONS.get(board_main, 'Unknown')})")
        print(f"    Sub Ver:    {board_sub}")
        print()
        print(f"  Region:       {region_info} ({REGIONS.get(region_info, 'Unknown')})")
        print()
        print(f"  Board Config: 0x{board_config:02X}")
        mode = "Service" if (board_config & 0x03) != 0x01 else "Retail"
        print(f"    Mode:       {mode}")
        print()

        language = data3[0]
        print(f"  Language:     {language} ({LANGUAGES.get(language, 'Unknown')})")

        dev_mode = data3[6]
        print(f"  Dev Mode:     {dev_mode}")

    def cmd_calibration_info(self):
        """Display calibration data from EEPROM"""
        print("\n--- Calibration Data ---\n")

        # Volume calibration
        vol = self.cmd_eeprom_read(0x121A, 2)
        print(f"  Volume: min={vol[0]}, max={vol[1]}")

        # Accelerometer
        accel = self.cmd_eeprom_read(0x1313, 12)
        print(f"  Accelerometer: {accel.hex().upper()}")

        # Gyroscope
        gyro = self.cmd_eeprom_read(0x122C, 19)
        print(f"  Gyroscope:     {gyro.hex().upper()}")

        # Touchscreen
        touch = self.cmd_eeprom_read(0x1344, 16)
        print(f"  Touchscreen:   {touch.hex().upper()}")

        # LCD brightness
        lcd = self.cmd_eeprom_read(0x1377, 13)
        print(f"  LCD Levels:    {lcd.hex().upper()}")
        lcd_setting = self.cmd_eeprom_read(0x1386, 1)[0]
        print(f"  LCD Setting:   {lcd_setting}")

    # ==================== Hardware Commands ====================

    def cmd_led(self, led, brightness):
        """Set LED brightness (0=home, 1=blue, 2=red, 3=amber)"""
        self.send_cmd(b'L')
        self.ser.write(bytes([led, brightness]))
        self.ser.flush()
        self.wait_for(b'OK\r\n')

    def cmd_rumble(self, on):
        """Set rumble on/off"""
        self.send_cmd(b'U')
        self.ser.write(bytes([1 if on else 0]))
        self.ser.flush()
        self.wait_for(b'OK\r\n')

    # ==================== Interactive Menu ====================

    def run_interactive(self):
        """Main interactive loop"""
        while True:
            choice = self._show_main_menu()

            if choice == '1':
                self._menu_flash_read()
            elif choice == '2':
                self._menu_flash_write()
            elif choice == '3':
                self._menu_dump_all()
            elif choice == '4':
                self._menu_boot_partition()
            elif choice == '5':
                self._menu_service_mode()
            elif choice == '6':
                self._menu_eeprom()
            elif choice == '7':
                self._menu_device_info()
            elif choice == '8':
                self._menu_memory()
            elif choice == '9':
                self._menu_hardware()
            elif choice == 'p':
                self.power_off()
                print("GamePad powered off.")
                break
            elif choice in ('0', 'q', 'Q'):
                print("\nExiting (GamePad still on)")
                break
            else:
                print("\nInvalid choice.")
                time.sleep(0.3)

    def _show_main_menu(self):
        print("\n" + "=" * 45)
        print("            WUP Tool")
        print("=" * 45)
        print()
        print(" FLASH")
        print("  [1] Read Partition")
        print("  [2] Write Partition")
        print("  [3] Dump All")
        print()
        print(" CONFIG")
        print("  [4] Boot Partition")
        print("  [5] Service Mode")
        print("  [6] EEPROM Browser")
        print()
        print(" DEBUG")
        print("  [7] Device Info")
        print("  [8] Memory Browser")
        print("  [9] Hardware Test")
        print()
        print("  [P] Power Off")
        print("  [0] Exit")
        print()
        print("-" * 45)

        try:
            return input("Select: ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            return '0'

    def _pause(self):
        print()
        try:
            input("Press Enter...")
        except (EOFError, KeyboardInterrupt):
            pass

    def _menu_device_info(self):
        print("\n--- Device Info ---")
        self.cmd_info()
        self._pause()

    def _menu_flash_read(self):
        print("\n--- Read Flash ---")
        print()
        parts = ['spl', 'product1', 'product2', 'lang1', 'lang2', 'tvcode', 'service', 'all']
        for i, p in enumerate(parts):
            addr, size = PARTITIONS[p]
            sz = f"{size//0x100000}MB" if size >= 0x100000 else f"{size//0x400}KB"
            print(f"  [{i+1}] {p:<12} {sz}")
        print(f"  [9] Custom address")
        print(f"  [0] Back")
        print()

        try:
            choice = input("Select: ").strip()
        except (EOFError, KeyboardInterrupt):
            return

        if choice == '0':
            return
        elif choice == '9':
            try:
                addr = parse_int(input("Address (hex): ").strip())
                length = parse_int(input("Length (hex): ").strip())
                name = 'custom'
            except:
                print("Invalid input")
                return
        elif choice.isdigit() and 1 <= int(choice) <= len(parts):
            name = parts[int(choice)-1]
            addr, length = PARTITIONS[name]
        else:
            return

        try:
            filename = input(f"Output file [{name}.bin]: ").strip() or f"{name}.bin"
        except:
            return

        print()
        self.cmd_read(addr, length, filename)
        self._pause()

    def _menu_flash_write(self):
        print("\n--- Write Flash ---")
        print()
        print("  !!! WARNING: Can brick GamePad !!!")
        print()

        try:
            filename = input("Input file: ").strip()
        except:
            return

        if not filename or not os.path.exists(filename):
            print("File not found")
            return

        file_size = os.path.getsize(filename)
        print(f"\nFile: {file_size} bytes")

        parts = ['spl', 'product1', 'product2', 'lang1', 'lang2', 'tvcode', 'service']
        for i, p in enumerate(parts):
            addr, size = PARTITIONS[p]
            sz = f"{size//0x100000}MB" if size >= 0x100000 else f"{size//0x400}KB"
            print(f"  [{i+1}] {p:<12} {sz}")
        print(f"  [8] Custom address")
        print(f"  [0] Cancel")
        print()

        try:
            choice = input("Destination: ").strip()
        except:
            return

        if choice == '0':
            return
        elif choice == '8':
            try:
                addr = parse_int(input("Address (hex): ").strip())
            except:
                return
        elif choice.isdigit() and 1 <= int(choice) <= len(parts):
            addr, _ = PARTITIONS[parts[int(choice)-1]]
        else:
            return

        print(f"\nWrite {filename} to 0x{addr:07X}?")
        try:
            if input("Type YES to confirm: ").strip() != 'YES':
                print("Cancelled")
                return
        except:
            return

        print()
        self.cmd_write(addr, filename)
        self._pause()

    def _menu_dump_all(self):
        print("\n--- Dump All Partitions ---")
        try:
            outdir = input("Output directory [flash_dump]: ").strip() or "flash_dump"
            if input(f"Dump to '{outdir}'? (yes/no): ").strip().lower() != 'yes':
                return
        except:
            return

        self.cmd_dump(outdir)
        self._pause()

    def _menu_boot_partition(self):
        print("\n--- Boot Partition ---")
        self.cmd_boot()
        print()
        print("  [1] Set to Product1 (0)")
        print("  [2] Set to Product2 (1)")
        print("  [0] Back")
        print()

        try:
            choice = input("Select: ").strip()
        except:
            return

        if choice == '1':
            self.cmd_boot(0)
        elif choice == '2':
            self.cmd_boot(1)
        self._pause()

    def _menu_service_mode(self):
        print("\n--- Service Mode ---")
        self.cmd_service_mode()
        print()
        print("  [1] Enable Service Mode")
        print("  [2] Restore Retail Mode")
        print("  [0] Back")
        print()

        try:
            choice = input("Select: ").strip()
        except:
            return

        if choice == '1':
            self.cmd_service_mode(enable=True)
        elif choice == '2':
            self.cmd_service_mode(enable=False)
        self._pause()

    def _menu_eeprom(self):
        print("\n--- EEPROM Browser ---")
        print()
        print("  [1] WiFi Info (SSID, PSK, Channel)")
        print("  [2] Board Info")
        print("  [3] Calibration Data")
        print("  [4] Dump Full EEPROM")
        print("  [5] Hexdump Region")
        print("  [6] Read Field")
        print("  [7] Write Field")
        print("  [0] Back")
        print()

        try:
            choice = input("Select: ").strip()
        except:
            return

        if choice == '1':
            self.cmd_wifi_info()
        elif choice == '2':
            self.cmd_board_info()
        elif choice == '3':
            self.cmd_calibration_info()
        elif choice == '4':
            try:
                filename = input("Output file [eeprom.bin]: ").strip() or "eeprom.bin"
            except:
                return
            self.cmd_eeprom_dump(filename)
        elif choice == '5':
            try:
                addr = parse_int(input("Address (hex, e.g. 0x1101): ").strip())
                length = parse_int(input("Length [0x100]: ").strip() or "0x100")
            except:
                return
            self.cmd_eeprom_hexdump(addr, min(length, 0x800))
        elif choice == '6':
            print("\n  Available fields:")
            fields = sorted(EEPROM_FIELDS.keys())
            for i, name in enumerate(fields):
                addr, length, fmt = EEPROM_FIELDS[name]
                print(f"    {name:<20} @ 0x{addr:04X} ({length} bytes)")
            print()
            try:
                field = input("Field name: ").strip()
            except:
                return
            if field in EEPROM_FIELDS:
                addr, length, fmt = EEPROM_FIELDS[field]
                data = self.cmd_eeprom_read(addr, length)
                if fmt == 'string':
                    # Find null terminator
                    try:
                        null_idx = data.index(0)
                        val = data[:null_idx].decode('ascii', errors='replace')
                    except ValueError:
                        val = data.decode('ascii', errors='replace')
                    print(f"  {field} = \"{val}\"")
                elif fmt == 'u8':
                    print(f"  {field} = {data[0]} (0x{data[0]:02X})")
                elif fmt == 'u16':
                    val = int.from_bytes(data, 'little')
                    print(f"  {field} = {val} (0x{val:04X})")
                elif fmt == 'mac':
                    print(f"  {field} = {':'.join(f'{b:02X}' for b in data)}")
                else:  # hex
                    print(f"  {field} = {data.hex().upper()}")
            else:
                print(f"Unknown field: {field}")
        elif choice == '7':
            print("\n  !!! WARNING: Can corrupt EEPROM !!!")
            print()
            try:
                addr = parse_int(input("Address (hex): ").strip())
                hex_data = input("Data (hex): ").strip()
                data = bytes.fromhex(hex_data)
            except Exception as e:
                print(f"Error: {e}")
                return
            print(f"\nWrite {len(data)} bytes to 0x{addr:04X}?")
            try:
                if input("Type YES to confirm: ").strip() != 'YES':
                    print("Cancelled")
                    return
            except:
                return
            self.cmd_eeprom_write(addr, data)
            print("OK")

        self._pause()

    def _menu_memory(self):
        print("\n--- Memory Browser ---")
        print()
        print("  [1] Dump bootrom (0x0, 4KB)")
        print("  [2] Dump RAM region")
        print("  [3] Read register")
        print("  [4] Write register")
        print("  [0] Back")
        print()

        try:
            choice = input("Select: ").strip()
        except:
            return

        if choice == '1':
            try:
                filename = input("Output file [bootrom.bin]: ").strip() or "bootrom.bin"
            except:
                return
            self.cmd_bootrom_dump(filename)
        elif choice == '2':
            try:
                addr = parse_int(input("Address (hex): ").strip())
                length = parse_int(input("Length [0x100]: ").strip() or "0x100")
            except:
                return
            self.cmd_mem_dump(addr, min(length, 0x1000))
        elif choice == '3':
            try:
                addr = parse_int(input("Address (hex): ").strip())
            except:
                return
            data = self.cmd_mem_read(addr, 4)
            val = int.from_bytes(data, 'little')
            print(f"0x{addr:08X} = 0x{val:08X}")
        elif choice == '4':
            try:
                addr = parse_int(input("Address (hex): ").strip())
                val = parse_int(input("Value (hex): ").strip())
            except:
                return
            self.cmd_mem_write(addr, val.to_bytes(4, 'little'))
            print("OK")

        self._pause()

    def _menu_hardware(self):
        print("\n--- Hardware Test ---")
        print()
        print("  [1] LED Control")
        print("  [2] Rumble Test")
        print("  [0] Back")
        print()

        try:
            choice = input("Select: ").strip()
        except:
            return

        if choice == '1':
            print("\n  LEDs: 0=Home, 1=Blue, 2=Red, 3=Amber")
            try:
                led = int(input("LED [0]: ").strip() or "0")
                brightness = int(input("Brightness 0-255 [255]: ").strip() or "255")
            except:
                return
            self.cmd_led(led, brightness)
            print("OK")
        elif choice == '2':
            print("\nRumble for 1 second...")
            self.cmd_rumble(True)
            time.sleep(1)
            self.cmd_rumble(False)
            print("Done")

        self._pause()


# ==================== CLI ====================

def print_usage():
    print("""
WUP Tool - Wii U GamePad Tool

Usage:
  wuptool.py                         Interactive mode (default)
  wuptool.py help                    Show this help

Flash Commands:
  wuptool.py info                    Device info
  wuptool.py read <part> <file>      Read partition to file
  wuptool.py write <part> <file>     Write file to partition
  wuptool.py patch <patchfile>       Apply patches (read-modify-write)
  wuptool.py dump <dir>              Dump all partitions
  wuptool.py boot [0|1]              Get/set boot partition

Config Commands:
  wuptool.py service                 Show service mode status
  wuptool.py service on              Enable service mode
  wuptool.py service off             Restore retail mode

EEPROM Commands:
  wuptool.py wifi                    Show WiFi config (SSID, PSK, channel)
  wuptool.py board                   Show board info (region, version)
  wuptool.py calibration             Show calibration data
  wuptool.py eeprom <file>           Dump full EEPROM to file
  wuptool.py eeread <addr> [len]     Read EEPROM (hex output)
  wuptool.py eewrite <addr> <hex>    Write EEPROM (dangerous!)

Memory Commands:
  wuptool.py bootrom <file>          Dump bootrom to file
  wuptool.py peek <addr>             Read 32-bit from address
  wuptool.py poke <addr> <val>       Write 32-bit to address
  wuptool.py memdump <addr> [len]    Hexdump memory

Hardware Commands:
  wuptool.py led <n> <brightness>    Set LED (0-3) brightness (0-255)
  wuptool.py rumble <on|off>         Rumble motor control
  wuptool.py poweroff                Power off GamePad

Partitions: spl, product1, product2, lang1, lang2, tvcode, service, all

Options:
  --port <dev>      Override serial port
  --no-poweroff     Don't power off after command

Examples:
  wuptool.py                         # Interactive menu
  wuptool.py read product1 fw.bin    # Dump firmware
  wuptool.py patch wifi_wpa2.patch   # Apply patches from file
  wuptool.py service on              # Enable service mode
  wuptool.py bootrom bootrom.bin     # Dump bootrom

Patch File Format:
  # Comments start with #
  address:find_hex:replace_hex

  Example (wifi_wpa2.patch):
    # WPA2 OUI patches for partition 2
    0x5E5B22:A4C0E1:000FAC
    0x5E731F:A4C0E1:000FAC
    0x5E7325:A4C0E1:000FAC
    0x5E732B:A4C0E1:000FAC
""")


def main():
    import argparse

    # Interactive mode if no args
    if len(sys.argv) == 1:
        print()
        print("=" * 45)
        print("         WUP Tool - GamePad Tool")
        print("=" * 45)
        print()

        port = find_serial_port()
        if not port:
            print("No USB serial port found!")
            sys.exit(1)

        wup = WUPTool(port)
        try:
            wup.connect()
            wup.run_interactive()
        except KeyboardInterrupt:
            print("\n\nAborted!")
        except Exception as e:
            print(f"\nError: {e}")
            raise
        finally:
            wup.close()
        return

    # CLI mode
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('command', nargs='?')
    parser.add_argument('args', nargs='*')
    parser.add_argument('--port', '-p')
    parser.add_argument('--no-poweroff', action='store_true')
    parser.add_argument('--help', '-h', action='store_true')

    args = parser.parse_args()

    if args.help or not args.command or args.command.lower() in ('help', '-h', '--help'):
        print_usage()
        sys.exit(0)

    port = args.port or find_serial_port()
    if not port:
        print("No USB serial port found!")
        sys.exit(1)

    cmd = args.command.lower()
    cmd_args = args.args

    wup = WUPTool(port)
    try:
        wup.connect()

        if cmd == 'info':
            wup.cmd_info()

        elif cmd == 'read':
            if len(cmd_args) < 2:
                print("Usage: wuptool.py read <partition> <file>")
                sys.exit(1)
            part, outfile = cmd_args[0], cmd_args[1]
            if part not in PARTITIONS:
                print(f"Unknown partition: {part}")
                sys.exit(1)
            addr, length = PARTITIONS[part]
            wup.cmd_read(addr, length, outfile)

        elif cmd == 'write':
            if len(cmd_args) < 2:
                print("Usage: wuptool.py write <partition> <file>")
                sys.exit(1)
            part, infile = cmd_args[0], cmd_args[1]
            if part not in PARTITIONS:
                print(f"Unknown partition: {part}")
                sys.exit(1)
            if not os.path.exists(infile):
                print(f"File not found: {infile}")
                sys.exit(1)
            addr, _ = PARTITIONS[part]
            wup.cmd_write(addr, infile)

        elif cmd == 'dump':
            if not cmd_args:
                print("Usage: wuptool.py dump <dir>")
                sys.exit(1)
            wup.cmd_dump(cmd_args[0])

        elif cmd == 'patch':
            if not cmd_args:
                print("Usage: wuptool.py patch <patchfile>")
                print()
                print("Patch file format (one per line):")
                print("  address:find_hex:replace_hex")
                print()
                print("Example:")
                print("  # WPA2 OUI patches for partition 2")
                print("  0x5E5B22:A4C0E1:000FAC")
                print("  0x5E731F:A4C0E1:000FAC")
                sys.exit(1)
            patchfile = cmd_args[0]
            if not os.path.exists(patchfile):
                print(f"File not found: {patchfile}")
                sys.exit(1)
            wup.cmd_patch(patchfile)

        elif cmd == 'boot':
            if cmd_args:
                part = int(cmd_args[0])
                if part not in [0, 1]:
                    print("Boot partition must be 0 or 1")
                    sys.exit(1)
                wup.cmd_boot(part)
            else:
                wup.cmd_boot()

        elif cmd == 'service':
            if not cmd_args:
                wup.cmd_service_mode()
            elif cmd_args[0].lower() in ('on', 'enable', '1'):
                wup.cmd_service_mode(enable=True)
            elif cmd_args[0].lower() in ('off', 'disable', '0', 'retail'):
                wup.cmd_service_mode(enable=False)
            else:
                print("Usage: wuptool.py service [on|off]")

        # EEPROM commands
        elif cmd == 'wifi':
            wup.cmd_wifi_info()

        elif cmd == 'board':
            wup.cmd_board_info()

        elif cmd == 'calibration':
            wup.cmd_calibration_info()

        elif cmd == 'eeprom':
            outfile = cmd_args[0] if cmd_args else 'eeprom.bin'
            wup.cmd_eeprom_dump(outfile)

        elif cmd == 'eeread':
            if not cmd_args:
                print("Usage: wuptool.py eeread <addr> [len]")
                sys.exit(1)
            addr = parse_int(cmd_args[0])
            length = parse_int(cmd_args[1]) if len(cmd_args) > 1 else 0x10
            wup.cmd_eeprom_hexdump(addr, min(length, 0x800))

        elif cmd == 'eewrite':
            if len(cmd_args) < 2:
                print("Usage: wuptool.py eewrite <addr> <hex_data>")
                sys.exit(1)
            addr = parse_int(cmd_args[0])
            data = bytes.fromhex(cmd_args[1])
            print(f"Writing {len(data)} bytes to EEPROM 0x{addr:04X}...")
            wup.cmd_eeprom_write(addr, data)
            print("OK")

        elif cmd == 'bootrom':
            outfile = cmd_args[0] if cmd_args else 'bootrom.bin'
            wup.cmd_bootrom_dump(outfile)

        elif cmd == 'flashread':
            if not cmd_args:
                print("Usage: wuptool.py flashread <addr> [len]")
                sys.exit(1)
            addr = parse_int(cmd_args[0])
            length = parse_int(cmd_args[1]) if len(cmd_args) > 1 else 16
            data = wup.cmd_read_data(addr, min(length, 256))
            print(f"Flash 0x{addr:06X} ({length} bytes):")
            # Hex dump
            for i in range(0, len(data), 16):
                hex_part = ' '.join(f'{b:02X}' for b in data[i:i+16])
                ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[i:i+16])
                print(f"  {addr+i:06X}: {hex_part:<48} {ascii_part}")

        elif cmd == 'peek':
            if not cmd_args:
                print("Usage: wuptool.py peek <addr>")
                sys.exit(1)
            addr = parse_int(cmd_args[0])
            data = wup.cmd_mem_read(addr, 4)
            val = int.from_bytes(data, 'little')
            print(f"0x{addr:08X} = 0x{val:08X}")

        elif cmd == 'poke':
            if len(cmd_args) < 2:
                print("Usage: wuptool.py poke <addr> <val>")
                sys.exit(1)
            addr = parse_int(cmd_args[0])
            val = parse_int(cmd_args[1])
            wup.cmd_mem_write(addr, val.to_bytes(4, 'little'))
            print("OK")

        elif cmd == 'memdump':
            if not cmd_args:
                print("Usage: wuptool.py memdump <addr> [len]")
                sys.exit(1)
            addr = parse_int(cmd_args[0])
            length = parse_int(cmd_args[1]) if len(cmd_args) > 1 else 0x100
            wup.cmd_mem_dump(addr, min(length, 0x1000))

        elif cmd == 'led':
            if len(cmd_args) < 2:
                print("Usage: wuptool.py led <0-3> <0-255>")
                sys.exit(1)
            wup.cmd_led(int(cmd_args[0]), int(cmd_args[1]))
            print("OK")

        elif cmd == 'rumble':
            if not cmd_args:
                print("Usage: wuptool.py rumble <on|off>")
                sys.exit(1)
            on = cmd_args[0].lower() in ('on', '1', 'true')
            wup.cmd_rumble(on)
            print("Rumble " + ("on" if on else "off"))

        elif cmd == 'poweroff':
            wup.power_off()
            args.no_poweroff = True

        else:
            print(f"Unknown command: {cmd}")
            print_usage()
            sys.exit(1)

        # Power off unless --no-poweroff
        if not args.no_poweroff:
            wup.power_off()

    except KeyboardInterrupt:
        print("\n\nAborted!")
    except Exception as e:
        print(f"\nError: {e}")
        raise
    finally:
        wup.close()


if __name__ == '__main__':
    main()
