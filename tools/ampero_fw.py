#!/usr/bin/env python3
"""Hotone Ampero II Stomp firmware unpacker / repacker.

Container formats (reverse engineered from HTUSBTools.dll, Ampero II editor):

  HTFW (V2.5.0+)   header + section table + LZO1X-compressed payload
                   (parsed by HTUSBTools.dll @ RVA 0xcc3a0, which converts it to FRMW)
  FRMW (<= V2.3.x) header + section table + plain payload; this is also what
                   the editor actually sends to the device

There is no encryption: the HTFW payload is a single miniLZO 2.10 stream.
"""
import argparse
import json
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzo1x  # noqa: E402

SECTION_NAMES = {
    0x00: 'arm',       # i.MX RT1052 application (ITCM part + SDRAM part)
    0x01: 'ir',        # factory IR library: 30 x (char name[32] + int32 ir[2048])
    0x02: 'presets',   # factory presets: 99 x 0x400 slots (LZO compressed)
    0x10: 'dsp',       # DSP firmware image
    0x11: 'dsp',
    0x20: 'ext20',     # not present in V2.5.0; DLL reads a "Vx.y.z" string at +0x30
    0x21: 'ext21',
}
# order of the sections in the legacy FRMW image built by the DLL
FRMW_ORDER = [0x00, 0x10, 0x01, 0x02, 0x20, 0x21]

HTFW_HDR = struct.Struct('<4sHHI16s4sBBBBI16s')   # 0x38 bytes
SEC_HEAD = struct.Struct('>HBB')   # crc16 (big endian), reserved, type
SEC_BODY = struct.Struct('<III')   # reserved, offset, size


def crc16_modbus(data):
    tab = _crc_tab()
    crc = 0xffff
    for b in data:
        crc = (crc >> 8) ^ tab[(crc ^ b) & 0xff]
    return crc


_TAB = None


def _crc_tab():
    global _TAB
    if _TAB is None:
        _TAB = []
        for i in range(256):
            c = i
            for _ in range(8):
                c = (c >> 1) ^ 0xa001 if c & 1 else c >> 1
            _TAB.append(c)
    return _TAB


# --------------------------------------------------------------------------- HTFW

def parse_htfw(buf):
    (magic, crc_unused, fmt, size, model, ver, b20, compressed, nsec, b23, plain_size,
     reserved) = HTFW_HDR.unpack_from(buf, 0)
    if magic != b'HTFW':
        raise ValueError('not an HTFW image')
    hdr = {
        'magic': 'HTFW',
        'header_crc16': struct.unpack_from('>H', buf, 4)[0],
        'format': fmt,
        'file_size': size,
        'model': model.split(b'\0')[0].decode('ascii', 'replace'),
        'version_bytes': ver.hex(),
        'version': '%s%d.%d.%d' % (chr(ver[0]), ver[1], ver[2], ver[3]),
        'byte20': b20,
        'compressed': compressed,
        'section_count': nsec,
        'byte23': b23,
        'plain_size': plain_size,
        'reserved': reserved.hex(),
    }
    secs = []
    o = HTFW_HDR.size
    for _ in range(nsec):
        crc, res, typ = SEC_HEAD.unpack_from(buf, o)
        res2, off, ln = SEC_BODY.unpack_from(buf, o + 4)
        secs.append({'type': typ, 'name': SECTION_NAMES.get(typ, 'type%02x' % typ),
                     'crc16': crc, 'res8': res, 'res32': res2, 'offset': off, 'size': ln})
        o += 16
    hdr['payload_offset'] = o
    return hdr, secs


def htfw_payload(buf, hdr):
    data = buf[hdr['payload_offset']:]
    if hdr['compressed'] != 1:
        return data
    usize = struct.unpack_from('<I', data, 0)[0]
    plain, used = lzo1x.decompress(data[4:], return_consumed=True)
    if len(plain) != usize:
        raise ValueError('LZO size mismatch: %d != %d' % (len(plain), usize))
    if 4 + used != len(data):
        print('warning: %d trailing bytes after LZO stream' % (len(data) - 4 - used))
    return plain


def build_htfw(hdr, sections, payload_plain):
    """Rebuild an HTFW file (same layout as the original)."""
    nsec = len(sections)
    tab = b''
    for s in sections:
        crc = crc16_modbus(payload_plain[s['offset']:s['offset'] + s['size']])
        tab += SEC_HEAD.pack(crc, s.get('res8', 0), s['type'])
        tab += SEC_BODY.pack(s.get('res32', 0), s['offset'], s['size'])
    if hdr['compressed'] == 1:
        payload = struct.pack('<I', len(payload_plain)) + lzo1x.compress(payload_plain)
    else:
        payload = payload_plain
    total = HTFW_HDR.size + len(tab) + len(payload)
    model = hdr['model'].encode().ljust(16, b'\0')
    head = HTFW_HDR.pack(b'HTFW', 0, hdr['format'], total, model, bytes.fromhex(hdr['version_bytes']),
                         hdr['byte20'], hdr['compressed'], nsec, hdr['byte23'], len(payload_plain),
                         bytes.fromhex(hdr['reserved']))
    out = bytearray(head + tab + payload)
    struct.pack_into('>H', out, 4, crc16_modbus(out[6:]))
    return bytes(out)


def build_frmw_like_dll(hdr, secs, plain):
    """Replicates HTUSBTools.dll 0x1800cc3a0: HTFW -> legacy FRMW image sent to the device."""
    by_type = {}
    for s in secs:
        t = 0x10 if s['type'] in (0x10, 0x11) else s['type']
        by_type[t] = plain[s['offset']:s['offset'] + s['size']]
    blobs = [by_type.get(t, b'') for t in FRMW_ORDER]
    total = 0x30 + sum(len(b) for b in blobs)
    ver = bytes.fromhex(hdr['version_bytes'])
    reserved = bytes.fromhex(hdr['reserved'])
    h = bytearray(0x30)
    h[0:4] = b'FRMW'
    struct.pack_into('<I', h, 4, total)
    struct.pack_into('<H', h, 8, hdr['byte20'])
    h[0xa] = ver[1]
    h[0xb] = ver[0]
    h[0x10:0x20] = hdr['model'].encode().ljust(16, b'\0')
    h[0x20] = reserved[0]
    h[0x21] = reserved[1]
    for t, pos in ((0x20, 0x22), (0x21, 0x26)):     # optional "Vx.y.z" strings
        b = by_type.get(t, b'')
        if len(b) > 0x34:
            h[pos] = 3 if b[0x30] == 0x56 else 0
            digits = [c - 0x30 for c in b[0x31:0x40] if c != 0x2e]
            for i, dgt in enumerate(digits[:3]):
                if not 0 <= dgt <= 9:
                    break
                h[pos + 1 + i] = dgt
    table = bytearray()
    off = 0x30
    for b in blobs:
        table += struct.pack('<II', off, len(b))
        off += len(b)
    img = bytearray(h + table + b''.join(blobs))
    struct.pack_into('<H', img, 0xe, sum(img[0x30:0x30 + total]) & 0xff)
    return bytes(img)


# --------------------------------------------------------------------------- FRMW

def parse_frmw(buf):
    if buf[:4] != b'FRMW':
        raise ValueError('not a FRMW image')
    total = struct.unpack_from('<I', buf, 4)[0]
    first = struct.unpack_from('<I', buf, 0x30)[0]
    n = first // 8
    secs = []
    types = [0x00, 0x10, 0x01, 0x02, 0x20, 0x21]
    for i in range(n):
        off, ln = struct.unpack_from('<II', buf, 0x30 + i * 8)
        t = types[i] if i < len(types) else 0x100 + i
        secs.append({'type': t, 'name': SECTION_NAMES.get(t, 'sec%d' % i),
                     'offset': 0x30 + off, 'size': ln})
    stored = struct.unpack_from('<H', buf, 0xe)[0]
    calc = sum(buf[0x30:0x30 + total]) & 0xff
    hdr = {'magic': 'FRMW', 'total': total, 'model': buf[0x10:0x20].split(b'\0')[0].decode(),
           'bytes_08_0b': buf[8:12].hex(), 'checksum': stored, 'checksum_ok': stored == calc}
    return hdr, secs


# --------------------------------------------------------------------------- section helpers

def split_arm(arm):
    """ARM section: 0x1c byte header, part A (vector table, runs from ITCM @0),
    part B (starts with 'Vx.y.z\\0\\0MODEL', runs from SDRAM)."""
    a_addr, a_size, b_addr, b_size = struct.unpack_from('<IIII', arm, 0)
    magic = arm[0x10:0x1c]
    a = arm[0x1c:0x1c + a_size]
    b_off = b_addr - a_addr + 0x1c
    b = arm[b_off:b_off + b_size]
    sp, reset = struct.unpack_from('<II', a, 0)
    info = {'flash_addr_a': hex(a_addr), 'size_a': a_size, 'flash_addr_b': hex(b_addr), 'size_b': b_size,
            'magic_ok': magic == b'\x55' * 4 + b'\xaa' * 4 + b'\xa5' * 4,
            'a_initial_sp': hex(sp), 'a_reset_vector': hex(reset),
            'b_version': b[:8].split(b'\0')[0].decode('ascii', 'replace'),
            'b_model': b[8:24].split(b'\0')[0].decode('ascii', 'replace'),
            'b_link_base_guess': '0x80dffe68 (b+0x198 == 0x80e00000, heuristic)'}
    return a, b, info


def split_ir(ir):
    out = []
    for i in range(0, len(ir) - 0x201f, 0x2020):
        name = ir[i:i + 32].split(b'\0')[0].decode('ascii', 'replace')
        samples = ir[i + 32:i + 0x2020]
        out.append((name, samples))
    return out


def split_factory_presets(sec):
    out = []
    for k in range(len(sec) // 0x400):
        slot = sec[k * 0x400:(k + 1) * 0x400]
        if slot[:4] == b'\xff' * 4:
            continue
        z0, csize, crc, z1, z2 = struct.unpack_from('<5I', slot)
        comp = slot[0x14:0x14 + csize]
        raw = lzo1x.decompress(comp)
        name = raw[4:21].split(b'\0')[0].decode('ascii', 'replace')
        out.append({'slot': k, 'name': name, 'comp_size': csize,
                    'crc_ok': zlib.crc32(comp) == crc, 'comp': comp, 'raw': raw})
    return out


def find_gifs(blob):
    res = []
    for m in re.finditer(rb'GIF8[79]a', blob):
        start = m.start()
        end = blob.find(b'\x00\x3b', start)  # last block terminator + trailer
        p = start + 13
        flags = blob[start + 10]
        if flags & 0x80:
            p += 3 * (2 << (flags & 7))
        try:
            while True:            # walk the block structure for an exact end
                c = blob[p]
                if c == 0x3b:
                    end = p + 1
                    break
                if c == 0x21:
                    p += 2
                elif c == 0x2c:
                    f = blob[p + 9]
                    p += 10
                    if f & 0x80:
                        p += 3 * (2 << (f & 7))
                    p += 1
                else:
                    raise ValueError
                while blob[p]:
                    p += blob[p] + 1
                p += 1
        except (IndexError, ValueError):
            end = end + 2 if end > 0 else start
        res.append((start, blob[start:end]))
    return res


# --------------------------------------------------------------------------- commands

def safe(name):
    return re.sub(r'[^A-Za-z0-9 _.+-]', '_', name).strip()


def cmd_info(args):
    buf = open(args.firmware, 'rb').read()
    if buf[:4] == b'HTFW':
        hdr, secs = parse_htfw(buf)
        hdr['header_crc_ok'] = crc16_modbus(buf[6:]) == hdr['header_crc16']
        hdr['file_size_ok'] = hdr['file_size'] == len(buf)
        print(json.dumps(hdr, indent=2))
        if args.verify:
            plain = htfw_payload(buf, hdr)
            for s in secs:
                s['crc_ok'] = crc16_modbus(plain[s['offset']:s['offset'] + s['size']]) == s['crc16']
        for s in secs:
            print('  type 0x%02x %-8s off 0x%08x size 0x%08x crc16 0x%04x%s' % (
                s['type'], s['name'], s['offset'], s['size'], s['crc16'],
                '' if 'crc_ok' not in s else (' OK' if s['crc_ok'] else ' BAD')))
    else:
        hdr, secs = parse_frmw(buf)
        print(json.dumps(hdr, indent=2))
        for s in secs:
            print('  %-8s off 0x%08x size 0x%08x' % (s['name'], s['offset'], s['size']))


def cmd_unpack(args):
    buf = open(args.firmware, 'rb').read()
    os.makedirs(args.outdir, exist_ok=True)
    manifest = {'source': os.path.basename(args.firmware)}
    if buf[:4] == b'HTFW':
        hdr, secs = parse_htfw(buf)
        if crc16_modbus(buf[6:]) != hdr['header_crc16']:
            print('WARNING: header CRC16 mismatch')
        plain = htfw_payload(buf, hdr)
        manifest['htfw'] = hdr
        sections = []
        for s in secs:
            s['data'] = plain[s['offset']:s['offset'] + s['size']]
            if crc16_modbus(s['data']) != s['crc16']:
                print('WARNING: CRC16 mismatch in section type 0x%02x' % s['type'])
            sections.append(s)
        if args.plain:
            open(os.path.join(args.outdir, 'payload_plain.bin'), 'wb').write(plain)
        if not args.no_frmw:
            frmw = build_frmw_like_dll(hdr, secs, plain)
            open(os.path.join(args.outdir, 'legacy_frmw.bin'), 'wb').write(frmw)
            print('legacy FRMW image (as sent by the editor): %d bytes' % len(frmw))
    else:
        hdr, secs = parse_frmw(buf)
        manifest['frmw'] = hdr
        sections = []
        for s in secs:
            s['data'] = buf[s['offset']:s['offset'] + s['size']]
            sections.append(s)

    manifest['sections'] = []
    for s in sections:
        fn = 'section_%02x_%s.bin' % (s['type'], s['name'])
        open(os.path.join(args.outdir, fn), 'wb').write(s['data'])
        m = {k: v for k, v in s.items() if k != 'data'}
        m['file'] = fn
        manifest['sections'].append(m)
        print('section type 0x%02x %-8s %9d bytes -> %s' % (s['type'], s['name'], len(s['data']), fn))
        data = s['data']
        if s['type'] == 0x00:
            a, b, info = split_arm(data)
            open(os.path.join(args.outdir, 'arm_a_itcm.bin'), 'wb').write(a)
            open(os.path.join(args.outdir, 'arm_b_sdram.bin'), 'wb').write(b)
            m['arm'] = info
            print('  ARM part A %d bytes (SP %s, reset %s), part B %d bytes (%s %s)' % (
                len(a), info['a_initial_sp'], info['a_reset_vector'], len(b), info['b_version'], info['b_model']))
            gifs = find_gifs(data)
            if gifs:
                gd = os.path.join(args.outdir, 'gif')
                os.makedirs(gd, exist_ok=True)
                for off, g in gifs:
                    open(os.path.join(gd, 'arm_%08x.gif' % off), 'wb').write(g)
                print('  %d GIF image(s) -> gif/' % len(gifs))
        elif s['type'] == 0x01:
            irs = split_ir(data)
            d = os.path.join(args.outdir, 'ir')
            os.makedirs(d, exist_ok=True)
            m['ir'] = []
            for i, (name, samples) in enumerate(irs):
                fn2 = '%02d_%s.s32' % (i, safe(name))
                open(os.path.join(d, fn2), 'wb').write(samples)
                m['ir'].append(name)
            print('  %d IRs (int32 LE, 2048 samples each) -> ir/' % len(irs))
        elif s['type'] == 0x02:
            pres = split_factory_presets(data)
            d = os.path.join(args.outdir, 'factory_presets')
            os.makedirs(d, exist_ok=True)
            m['presets'] = []
            for p in pres:
                fn2 = '%03d_%s.raw' % (p['slot'], safe(p['name']))
                open(os.path.join(d, fn2), 'wb').write(p['raw'])
                m['presets'].append({'slot': p['slot'], 'name': p['name'], 'crc_ok': p['crc_ok']})
            bad = sum(not p['crc_ok'] for p in pres)
            print('  %d factory presets (%d CRC errors) -> factory_presets/' % (len(pres), bad))
    with open(os.path.join(args.outdir, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=2)


def cmd_pack(args):
    man = json.load(open(os.path.join(args.indir, 'manifest.json')))
    if 'htfw' not in man:
        raise SystemExit('pack needs a manifest from an HTFW image')
    hdr = man['htfw']
    secs = sorted(man['sections'], key=lambda s: s['offset'])
    plain = bytearray()
    for s in secs:
        data = open(os.path.join(args.indir, s['file']), 'rb').read()
        s['offset'] = len(plain)
        s['size'] = len(data)
        plain += data
    out = build_htfw(hdr, secs, bytes(plain))
    open(args.output, 'wb').write(out)
    print('wrote %s (%d bytes, payload %d bytes)' % (args.output, len(out), len(plain)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    p = sp.add_parser('info', help='show header / sections')
    p.add_argument('firmware')
    p.add_argument('--verify', action='store_true', help='decompress and check section CRCs')
    p.set_defaults(func=cmd_info)
    p = sp.add_parser('unpack', help='decompress and split into sections')
    p.add_argument('firmware')
    p.add_argument('outdir')
    p.add_argument('--plain', action='store_true', help='also write the whole decompressed payload')
    p.add_argument('--no-frmw', action='store_true', help='do not build the legacy FRMW image')
    p.set_defaults(func=cmd_unpack)
    p = sp.add_parser('pack', help='rebuild an HTFW image from an unpack directory')
    p.add_argument('indir')
    p.add_argument('output')
    p.set_defaults(func=cmd_pack)
    args = ap.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
