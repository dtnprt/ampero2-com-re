#!/usr/bin/env python3
"""Hotone Ampero II Stomp – USB-MIDI SysEx protocol decoder / encoder.

Message (one SysEx, max 384 bytes):
  F0 21 25 4D 50 00 00 | cs | kind | totLo totHi | offLo offHi | nibbles … | F7
    cs    = sum(all nibbles) & 0x7F  (verified for host->device; device->host uses
            another, unknown rule – the editor only logs a mismatch, see FORMAT.md)
    kind  = 0x11 request/get, 0x12 set / reply
    tot   = total stream length in bytes (7-bit split), off = byte offset of this chunk
    nibbles: one byte = hi nibble, lo nibble; max 185 bytes per message
Stream (all chunks concatenated):
  u32 cmd | u32 len | data[len]   – data is ALWAYS an LZO1X-1 stream (also for 1-byte payloads)

Inputs: Wireshark/USBPcap .pcapng (Ampero = VID 0x84EF), .syx / binary with F0..F7,
or a text file with hex dumps (one SysEx per line).

  ampero_sysex.py decode capture.pcapng [--dump DIR] [--all]
  ampero_sysex.py encode <cmd> [<hex payload>] [--kind 0x12]   -> SysEx hex lines
  ampero_sysex.py scene 2                                       -> SysEx for "select scene 3"
"""
import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzo1x  # noqa: E402

HEADER = bytes.fromhex('f021254d500000')
ALT_HEADER = bytes.fromhex('f021257f4d50012c')     # short frames (cmd 0x3800 etc.), not decoded here
CHUNK = 185
HOTONE_VID = 0x84EF

CMDS = {
    0x05000017: 'connect / handshake',
    0x00090000: 'current preset (u32 index) get/set',
    0x01000000: 'preset dump: req u32 index -> u32 index + TLV preset (5068 B)',
    0x01080000: 'preset list (u16 order[300], char name[300][17], ...)',
    0x03000001: 'current scene (set: u8 scene, reply: u32)',
    0x03000003: 'status notification (u32)',
    0x03000005: 'firmware version string',
    0x0300000a: 'table (624 x u16) ?',
    0x04000003: 'user IR names (50 x char[32])',
    0x07000003: 'name list 540 B ?',
    0x08000003: 'clone/NAM names 540 B ?',
    0x05000013: '?',
    0x00070000: 'global settings ? (ends with u16 tempo 120)',
    0x000a0000: '?',
    0x010a0003: '?',
    0x01030000: 'footswitch functions = preset TLV tag 3 (get, 8 B)',
    0x01030001: 'footswitch functions set: u8 fs_func[3] (FS1..FS3)',
    0x01070001: 'footswitch -> slots set: u8 fs, u8 slot_on[12]',
    0x010b0001: 'footswitch setting set ? (11 B)',
}


# --------------------------------------------------------------------------- input

def pcapng_usb(path, vid=HOTONE_VID, device=None):
    """Yield (time, direction, usb_midi_bytes) for bulk transfers of the Ampero (USBPcap)."""
    blocks = []
    with open(path, 'rb') as f:
        while True:
            h = f.read(8)
            if len(h) < 8:
                break
            bt, bl = struct.unpack('<II', h)
            body = f.read(bl - 8)
            if bt == 6:
                _, th, tl, cap, _ = struct.unpack_from('<IIIII', body, 0)
                blocks.append((((th << 32) | tl) / 1e6, body[20:20 + cap]))
    if device is None:   # find device address by VID in a GET_DESCRIPTOR(DEVICE) response
        for _, d in blocks:
            hl, _, _, _, info, _, dev, _, tt, dl = struct.unpack_from('<HQIHBHHBBI', d, 0)
            p = d[hl:hl + dl]
            if tt == 2 and len(p) >= 18 and p[0] == 18 and p[1] == 1 and struct.unpack_from('<H', p, 8)[0] == vid:
                device = dev
                break
        if device is None:
            raise SystemExit('no device with VID 0x%04x found in capture' % vid)
    for t, d in blocks:
        hl, _, _, _, info, _, dev, ep, tt, dl = struct.unpack_from('<HQIHBHHBBI', d, 0)
        if dev == device and tt == 3 and dl:
            yield t, ('IN ' if ep & 0x80 else 'OUT'), d[hl:hl + dl]


def usb_midi_to_sysex(packets):
    """USB-MIDI event packets (4 bytes, CIN 4..7) -> list of (time, dir, sysex bytes)."""
    nbytes = {4: 3, 5: 1, 6: 2, 7: 3}
    buf, out = {}, []
    for t, dr, d in packets:
        for i in range(0, len(d) - 3, 4):
            cin = d[i] & 0xf
            if cin in nbytes:
                b = buf.setdefault(dr, bytearray())
                b += d[i + 1:i + 1 + nbytes[cin]]
                if cin != 4:
                    out.append((t, dr, bytes(b)))
                    buf[dr] = bytearray()
    return out


def load_messages(path):
    data = open(path, 'rb').read()
    if data[:4] == b'\x0a\x0d\x0d\x0a':
        return usb_midi_to_sysex(pcapng_usb(path))
    if b'\xf0' not in data[:1] and re.fullmatch(rb'[\s0-9a-fA-F:,-]*', data):
        msgs = []
        for i, line in enumerate(data.decode().splitlines()):
            h = re.sub(r'[^0-9a-fA-F]', '', line)
            if h:
                msgs.append((float(i), '?  ', bytes.fromhex(h)))
        return msgs
    return [(float(i), '?  ', m.group()) for i, m in enumerate(re.finditer(rb'\xf0[^\xf7]*\xf7', data, re.S))]


# --------------------------------------------------------------------------- framing

def parse_frame(x):
    if not x.startswith(HEADER) or len(x) < 14 or x[-1] != 0xf7:
        return None
    nib = x[13:-1]
    return {'cs': x[7], 'kind': x[8], 'total': x[9] | x[10] << 7, 'offset': x[11] | x[12] << 7,
            'cs_ok': (sum(nib) & 0x7f) == x[7],
            'data': bytes((nib[i] << 4) | nib[i + 1] for i in range(0, len(nib) - 1, 2))}


def reassemble(msgs):
    """-> list of transfers {time, dir, kind, cmd, payload(decompressed), raw_frames, cs_ok}"""
    cur, out = {}, []
    for t, dr, x in msgs:
        fr = parse_frame(x)
        if fr is None:
            out.append({'time': t, 'dir': dr, 'other': x})
            continue
        if fr['offset'] == 0:
            cur[dr] = {'time': t, 'dir': dr, 'kind': fr['kind'], 'total': fr['total'],
                       'buf': bytearray(), 'frames': [], 'cs_ok': True}
        tr = cur.get(dr)
        if tr is None or fr['offset'] != len(tr['buf']):
            continue
        tr['buf'] += fr['data']
        tr['frames'].append(x)
        tr['cs_ok'] &= fr['cs_ok']
        if len(tr['buf']) >= tr['total']:
            del cur[dr]
            b = bytes(tr.pop('buf'))
            cmd, ln = struct.unpack_from('<II', b)
            tr['cmd'] = cmd
            tr['lzo'] = b[8:8 + ln]
            tr['payload'] = lzo1x.decompress(tr['lzo']) if ln else b''
            out.append(tr)
    return out


def encode(cmd, payload=b'', kind=0x12):
    """Build the SysEx messages for one transfer (exactly like the editor)."""
    lzo = lzo1x.compress(payload) if payload else b''
    stream = struct.pack('<II', cmd, len(lzo)) + lzo
    msgs = []
    for off in range(0, len(stream), CHUNK):
        part = stream[off:off + CHUNK]
        nib = bytearray()
        for b in part:
            nib += bytes((b >> 4, b & 0xf))
        tot = len(stream)
        msgs.append(HEADER + bytes((sum(nib) & 0x7f, kind, tot & 0x7f, tot >> 7, off & 0x7f, off >> 7))
                    + bytes(nib) + b'\xf7')
    return msgs


# --------------------------------------------------------------------------- pretty printing

def describe(tr):
    cmd, p = tr['cmd'], tr['payload']
    if cmd == 0x03000001 and len(p) in (1, 4):
        return 'scene %d' % p[0]
    if cmd in (0x00090000, 0x01000000) and len(p) == 4:
        return 'preset index %d' % struct.unpack('<I', p)[0]
    if cmd == 0x01000000 and len(p) > 8:
        idx = struct.unpack_from('<I', p)[0]
        name = p[4 + 8 + 10 + 8 + 4:4 + 8 + 10 + 8 + 4 + 17].split(b'\0')[0].decode('ascii', 'replace')
        return 'preset #%d "%s" (%d B TLV)' % (idx, name, len(p) - 4)
    if cmd == 0x03000005:
        return p.split(b'\0')[0].decode('ascii', 'replace')
    if cmd in (0x01030000, 0x01030001):
        return 'FS functions ' + ' '.join('%02x' % b for b in p[:3]) + (' | ' + p[3:].hex(' ') if len(p) > 3 else '')
    if cmd == 0x01070001 and len(p) == 13:
        return 'FS%d -> slots %s' % (p[0] + 1, [i for i in range(12) if p[1 + i]])
    if len(p) <= 24:
        return p.hex(' ')
    return '%d bytes' % len(p)


def cmd_decode(a):
    msgs = load_messages(a.input)
    trs = reassemble(msgs)
    t0 = trs[0]['time'] if trs else 0
    if a.dump:
        os.makedirs(a.dump, exist_ok=True)
    for i, tr in enumerate(trs):
        if 'other' in tr:
            if a.all:
                print('%8.3f %s other  %s' % (tr['time'] - t0, tr['dir'], tr['other'].hex()))
            continue
        cs = '' if tr['cs_ok'] else ' (cs?)'
        print('%8.3f %s %02x %08x %-48s %s%s' % (tr['time'] - t0, tr['dir'], tr['kind'], tr['cmd'],
                                                CMDS.get(tr['cmd'], '?')[:48], describe(tr), cs))
        if a.dump and len(tr['payload']) > 24:
            fn = os.path.join(a.dump, '%03d_%s_%08x.bin' % (i, tr['dir'].strip().lower(), tr['cmd']))
            open(fn, 'wb').write(tr['payload'])


def cmd_encode(a):
    for m in encode(int(a.cmd, 16), bytes.fromhex(a.payload or ''), int(a.kind, 0)):
        print(m.hex(' '))


def cmd_scene(a):
    for m in encode(0x03000001, bytes([a.scene])):
        print(m.hex(' '))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    p = sp.add_parser('decode')
    p.add_argument('input')
    p.add_argument('--dump', help='write decompressed payloads > 24 B into this directory')
    p.add_argument('--all', action='store_true', help='also list non-Ampero / short-header SysEx')
    p.set_defaults(func=cmd_decode)
    p = sp.add_parser('encode')
    p.add_argument('cmd', help='hex, e.g. 03000001')
    p.add_argument('payload', nargs='?', help='uncompressed payload as hex')
    p.add_argument('--kind', default='0x12')
    p.set_defaults(func=cmd_encode)
    p = sp.add_parser('scene')
    p.add_argument('scene', type=int, help='0..2')
    p.set_defaults(func=cmd_scene)
    a = ap.parse_args()
    a.func(a)


if __name__ == '__main__':
    main()
