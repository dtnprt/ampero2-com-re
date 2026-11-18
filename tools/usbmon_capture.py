#!/usr/bin/env python3
"""Capture the Ampero's USB-MIDI traffic on Linux via usbmon and write it as a sniffer trace.

Output: one line per MIDI message, "<ms> IN|OUT <hex bytes>" - the app trace format the web sniffer
(and tools/ampero_sysex.py, as a hex dump) can open. IN = device -> host, OUT = host -> device.
Works for any program talking to the pedal (Hotone editor under Wine, the web editor, ...).

Needs read access to /dev/usbmonN (N = USB bus), e.g.
    sudo modprobe usbmon
    sudo setfacl -m u:$USER:r /dev/usbmon1

Usage: usbmon_capture.py [-o trace.txt] [--bus N] [--dev N]   (Ctrl+C stops; default: find the Ampero)
"""
import argparse
import glob
import os
import struct
import sys
import time

HOTONE_VID = '84ef'
HDR = struct.Struct('<QBBBBHbbqiiII8s')  # struct mon_bin_hdr (48 bytes, legacy read API)
CIN_LEN = {0x4: 3, 0x5: 1, 0x6: 2, 0x7: 3, 0x8: 3, 0x9: 3, 0xa: 3, 0xb: 3, 0xc: 2, 0xd: 2, 0xe: 3, 0xf: 1}


def find_ampero():
    for d in glob.glob('/sys/bus/usb/devices/*'):
        try:
            if open(os.path.join(d, 'idVendor')).read().strip() == HOTONE_VID:
                return int(open(os.path.join(d, 'busnum')).read()), int(open(os.path.join(d, 'devnum')).read())
        except OSError:
            pass
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-o', '--out', help='trace file (default: ampero_trace_<date>.txt)')
    ap.add_argument('--bus', type=int)
    ap.add_argument('--dev', type=int)
    a = ap.parse_args()
    bus, dev = a.bus, a.dev
    if bus is None or dev is None:
        found = find_ampero()
        if not found:
            sys.exit('Ampero not found (USB vendor 84ef) - plugged in?')
        bus, dev = bus or found[0], dev or found[1]
    out = a.out or time.strftime('ampero_trace_%Y-%m-%d-%H-%M-%S.txt')
    try:
        mon = open(f'/dev/usbmon{bus}', 'rb', buffering=0)
    except OSError as e:
        sys.exit(f'{e}\n-> sudo modprobe usbmon && sudo setfacl -m u:$USER:r /dev/usbmon{bus}')
    print(f'capturing bus {bus} device {dev} -> {out} (Ctrl+C stops)', file=sys.stderr)
    t0 = None
    buf = {'IN': [], 'OUT': []}
    n = 0
    with open(out, 'w') as f:
        try:
            while True:
                ev = mon.read(HDR.size + 65536)
                if len(ev) < HDR.size:
                    continue
                (_id, typ, xfer, ep, devnum, busnum, _fs, _fd, sec, usec, status, _lurb, lcap, _setup) = HDR.unpack_from(ev)
                if devnum != dev or xfer not in (1, 3):  # interrupt / bulk only
                    continue
                d_in = bool(ep & 0x80)
                # OUT data travels with the submission ('S', status -EINPROGRESS), IN data with the
                # successful completion ('C', status 0)
                if d_in and (typ != ord('C') or status != 0):
                    continue
                if not d_in and typ != ord('S'):
                    continue
                data = ev[HDR.size:HDR.size + lcap]
                t = sec * 1000 + usec / 1000
                if t0 is None:
                    t0 = t
                direction = 'IN' if d_in else 'OUT'
                for k in range(0, len(data) - 3, 4):  # USB-MIDI event packets
                    cin = data[k] & 0x0f
                    ln = CIN_LEN.get(cin)
                    if not ln:
                        continue
                    msg = data[k + 1:k + 1 + ln]
                    if cin == 0x4:  # SysEx continues
                        buf[direction] += msg
                        continue
                    if cin in (0x5, 0x6, 0x7):  # SysEx ends
                        msg = bytes(buf[direction]) + msg
                        buf[direction] = []
                    f.write(f'{t - t0:.1f} {direction} {msg.hex(" ")}\n')
                    n += 1
                f.flush()
        except KeyboardInterrupt:
            pass
    print(f'{n} messages written to {out}', file=sys.stderr)


if __name__ == '__main__':
    main()
