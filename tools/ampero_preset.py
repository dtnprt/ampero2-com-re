#!/usr/bin/env python3
"""Hotone Ampero II (Stomp) preset tool.

File layers (editor export):
  .prst  = base64( .arr )
  .arr   = JSON array of byte values, e.g. "[0,20,0,...]"
  .bin   = those bytes = raw LZO1X-1 stream (miniLZO, HTUSBTools.dll toolStartCompress)
  raw    = decompressed preset, 5004 bytes, TLV chain [u32 tag][u32 len][data] (tags 0..9)

Factory presets inside the firmware (section type 2) use a different, flat
8700-byte layout (see FORMAT.md); they can be decoded too (read-only).

Commands:
  decode <file> [-o out.json] [--raw out.raw]     any layer -> JSON (+ summary)
  build  <file.json> -o <out.prst|.arr|.bin|.raw> JSON -> preset
  convert <in> -o <out.prst|.arr|.bin|.raw>        convert between layers
"""
import argparse
import base64
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzo1x  # noqa: E402

# The effect database comes with the Hotone editor (flutter_assets/assets/data/v*_alg_data.json) and is not part of
# this repository: pass it with --alg-db or set AMPERO_ALG_DB. Without it, effects are shown by their codes only.
DEFAULT_ALG_DB = os.environ.get('AMPERO_ALG_DB') or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', '..', 'private', 'windows', 'windows software',
    'data', 'flutter_assets', 'assets', 'data', 'v1.0.9_alg_data.json')
N_SCENES, N_SLOTS, N_PARAMS = 3, 12, 25
EMPTY_ALG = 0xffffffff


# --------------------------------------------------------------------------- layers

def load_any(path):
    """Return (raw_bytes, layer) for .prst/.arr/.bin/raw input."""
    data = open(path, 'rb').read()
    s = data.strip()
    if s[:1] == b'[':
        return lzo1x.decompress(bytes(json.loads(s))), 'arr'
    if s and all(c in b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=\r\n' for c in s):
        arr = base64.b64decode(s)
        if arr.strip()[:1] == b'[':
            return lzo1x.decompress(bytes(json.loads(arr))), 'prst'
    if is_tlv(data) or len(data) == 8700:
        return data, 'raw'
    if is_tlv(data[4:]):                     # SysEx dump (cmd 0x01000000): u32 preset index + TLV
        return data[4:], 'sysex'
    return lzo1x.decompress(data), 'bin'


def save_any(raw, path):
    ext = os.path.splitext(path)[1].lower()
    if ext == '.raw':
        out = raw
    else:
        comp = lzo1x.compress(raw)
        if ext == '.bin':
            out = comp
        else:
            arr = ('[' + ','.join(str(b) for b in comp) + ']').encode()
            out = arr if ext == '.arr' else base64.b64encode(arr)
    open(path, 'wb').write(out)


# --------------------------------------------------------------------------- helpers

def is_tlv(d):
    o = 0
    while o + 8 <= len(d):
        t, ln = struct.unpack_from('<II', d, o)
        if t > 0xff:
            return False
        o += 8 + ln
    return o == len(d) and len(d) > 0


NEG_ZERO = struct.pack('<f', -0.0)


def f2j(v_bytes):
    """float32 -> JSON value; non-finite values are kept as their u32 hex pattern."""
    v = struct.unpack('<f', v_bytes)[0]
    if not math.isfinite(v):
        return '0x%08x' % struct.unpack('<I', v_bytes)[0]
    if v == int(v) and v_bytes != NEG_ZERO:
        return int(v)
    for digits in range(6, 10):   # shortest decimal that maps back to the same float32
        s = float('%.*g' % (digits, v))
        if struct.pack('<f', s) == v_bytes:
            return s
    return v


def j2f(v):
    if isinstance(v, str):
        return struct.pack('<I', int(v, 16))
    return struct.pack('<f', v)


def str_field(b):
    text = b.split(b'\0')[0]
    canon = text + b'\0' * (len(b) - len(text))
    try:
        t = text.decode('ascii')
    except UnicodeDecodeError:
        return {'hex': b.hex()}
    return t if canon == b else {'text': t, 'hex': b.hex()}


def str_bytes(v, n):
    if isinstance(v, dict):
        return bytes.fromhex(v['hex'])
    return v.encode('ascii')[:n].ljust(n, b'\0')


# --------------------------------------------------------------------------- algorithm DB

class AlgDB:
    def __init__(self, path):
        self.algs = {}
        if not path or not os.path.exists(path):
            return
        j = json.load(open(path, encoding='utf-8'))
        for cat in j['PluginProperties']['Modules']['Catalog']:
            for a in cat['Alg']:
                ws = a.get('Widget', [])
                ws = [ws] if isinstance(ws, dict) else [w for w in ws if isinstance(w, dict)]
                self.algs[int(a['Code'])] = {'module': cat['Name'], 'name': a['Name'],
                                             'widgets': {int(w['ID']): w for w in ws}}

    def describe(self, code, params):
        if code == EMPTY_ALG:
            return None
        a = self.algs.get(code)
        if not a:
            return {'alg': '0x%08x' % code, 'name': '?', 'params': params[:8]}
        named = {}
        for i, w in sorted(a['widgets'].items()):
            if i < len(params):
                named[w['Name']] = params[i]
        return {'alg': '0x%08x' % code, 'module': a['module'], 'name': a['name'], 'params': named}


# --------------------------------------------------------------------------- 5004-byte TLV preset

def tlv_split(raw):
    out = []
    o = 0
    while o < len(raw):
        t, ln = struct.unpack_from('<II', raw, o)
        out.append((t, raw[o + 8:o + 8 + ln]))
        o += 8 + ln
    return out


def dec_info(d):
    return {'version': struct.unpack_from('<I', d, 0)[0], 'name': str_field(d[4:21]),
            'unknown_21': d[21:91].hex(), 'style': str_field(d[91:101])} if len(d) == 101 else {'hex': d.hex()}


def enc_info(v):
    if 'hex' in v:
        return bytes.fromhex(v['hex'])
    return (struct.pack('<I', v['version']) + str_bytes(v['name'], 17) + bytes.fromhex(v['unknown_21'])
            + str_bytes(v['style'], 10))


def dec_slots(d):
    if len(d) != 2 * N_SLOTS + 4 * N_SLOTS:
        return {'hex': d.hex()}
    return {'order': list(d[0:12]), 'chain': list(d[12:24]),
            'alg': ['0x%08x' % x for x in struct.unpack_from('<12I', d, 24)]}


def enc_slots(v):
    if 'hex' in v:
        return bytes.fromhex(v['hex'])
    return bytes(v['order']) + bytes(v['chain']) + struct.pack('<12I', *[int(x, 16) for x in v['alg']])


def dec_scenes(d):
    pbytes = N_SCENES * N_SLOTS * N_PARAMS * 4
    if len(d) < pbytes + 36 + 12 + 3 + 24:
        return {'hex': d.hex()}
    scenes = []
    p = pbytes
    en = d[p:p + 36]
    vol = struct.unpack_from('<3H', d, p + 36)
    tempo = struct.unpack_from('<3H', d, p + 42)
    names = d[-24:]
    for s in range(N_SCENES):
        params = []
        for k in range(N_SLOTS):
            o = (s * N_SLOTS + k) * N_PARAMS * 4
            params.append([f2j(d[o + 4 * i:o + 4 * i + 4]) for i in range(N_PARAMS)])
        scenes.append({'name': str_field(names[8 * s:8 * s + 8]), 'volume': vol[s], 'tempo': tempo[s],
                       'enabled': list(en[12 * s:12 * s + 12]), 'params': params})
    return {'scenes': scenes, 'unknown_after_tempo': d[p + 48:len(d) - 24].hex()}


def enc_scenes(v):
    if 'hex' in v:
        return bytes.fromhex(v['hex'])
    sc = v['scenes']
    out = bytearray()
    for s in sc:
        for slot in s['params']:
            for x in slot:
                out += j2f(x)
    for s in sc:
        out += bytes(s['enabled'])
    out += struct.pack('<3H', *[s['volume'] for s in sc])
    out += struct.pack('<3H', *[s['tempo'] for s in sc])
    out += bytes.fromhex(v['unknown_after_tempo'])
    for s in sc:
        out += str_bytes(s['name'], 8)
    return bytes(out)


TAGS = {
    0: ('header', lambda d: {'bytes': list(d)}, lambda v: bytes(v['bytes'])),
    1: ('info', dec_info, enc_info),
    2: ('tag2', None, None),
    3: ('footswitch', lambda d: {'fs_func': list(d[:3]), 'rest': d[3:].hex()},
        lambda v: bytes(v['fs_func']) + bytes.fromhex(v['rest'])),
    4: ('slots', dec_slots, enc_slots),
    5: ('scenes', dec_scenes, enc_scenes),
    6: ('tag6_exp', None, None),
    7: ('tag7', None, None),
    8: ('tag8_fs_slots', None, None),
    9: ('tag9_midi', None, None),
}


def decode_tlv(raw):
    blocks = []
    for t, d in tlv_split(raw):
        name, dec, _ = TAGS.get(t, ('tag%d' % t, None, None))
        blocks.append({'tag': t, 'name': name, 'data': dec(d) if dec else {'hex': d.hex()}})
    return {'format': 'tlv5004', 'blocks': blocks}


def encode_tlv(j):
    out = bytearray()
    for b in j['blocks']:
        _, _, enc = TAGS.get(b['tag'], (None, None, None))
        d = enc(b['data']) if enc and 'hex' not in b['data'] else bytes.fromhex(b['data']['hex'])
        out += struct.pack('<II', b['tag'], len(d)) + d
    return bytes(out)


def summary_tlv(j, db):
    blk = {b['tag']: b['data'] for b in j['blocks']}
    info, slots, sc = blk.get(1, {}), blk.get(4, {}), blk.get(5, {})
    nm = info.get('name')
    res = {'name': nm if isinstance(nm, str) else (nm or {}).get('text'),
           'style': info.get('style') if isinstance(info.get('style'), str) else None,
           'fs_func': ['0x%02x' % x for x in blk.get(3, {}).get('fs_func', [])]}
    if 'alg' in slots and 'scenes' in sc:
        res['slot_order'] = slots['order']
        res['scenes'] = []
        for s in sc['scenes']:
            sl = []
            for i, code in enumerate(slots['alg']):
                dsc = db.describe(int(code, 16), s['params'][i])
                if dsc:
                    dsc['slot'] = i
                    dsc['enabled'] = bool(s['enabled'][i])
                    sl.append(dsc)
            res['scenes'].append({'name': s['name'] if isinstance(s['name'], str) else s['name'].get('text'),
                                  'volume': s['volume'], 'tempo': s['tempo'], 'slots': sl})
    return res


# --------------------------------------------------------------------------- 8700-byte factory preset

def decode_factory(raw, db):
    codes = struct.unpack_from('<12I', raw, 0xa0)
    slots = []
    for i, c in enumerate(codes):
        o = 0xd8 + i * 0x64
        params = [f2j(raw[o + 4 * k:o + 4 * k + 4]) for k in range(N_PARAMS)]
        d = db.describe(c, params)
        if d:
            d['slot'] = i
            slots.append(d)
    return {'format': 'factory8700', 'info': dec_info(raw[0:101]),
            'chain_0x65': raw[0x65:0xa0].hex(), 'slots': slots,
            'note': 'only the documented fields are decoded; build is not supported for this layout'}


# --------------------------------------------------------------------------- commands

def cmd_decode(a):
    raw, layer = load_any(a.input)
    db = AlgDB(a.alg_db)
    if a.raw:
        open(a.raw, 'wb').write(raw)
    if is_tlv(raw):
        j = decode_tlv(raw)
        assert encode_tlv(j) == raw, 'internal roundtrip error'
        j['summary'] = summary_tlv(j, db)
    elif len(raw) == 8700:
        j = decode_factory(raw, db)
    else:
        raise SystemExit('unknown raw layout (%d bytes)' % len(raw))
    j['source_layer'] = layer
    j['raw_size'] = len(raw)
    txt = json.dumps(j, indent=1, ensure_ascii=False)
    if a.output:
        open(a.output, 'w', encoding='utf-8').write(txt)
        print('%s (%s, %d bytes raw) -> %s' % (a.input, layer, len(raw), a.output))
    else:
        print(json.dumps(j.get('summary', j), indent=1, ensure_ascii=False))


def cmd_build(a):
    j = json.load(open(a.input, encoding='utf-8'))
    if j.get('format') != 'tlv5004':
        raise SystemExit('only tlv5004 JSON can be built')
    raw = encode_tlv(j)
    save_any(raw, a.output)
    print('built %s (%d bytes raw)' % (a.output, len(raw)))


def cmd_convert(a):
    raw, layer = load_any(a.input)
    save_any(raw, a.output)
    print('%s (%s) -> %s' % (a.input, layer, a.output))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--alg-db', default=DEFAULT_ALG_DB, help='alg_data JSON from the editor (flutter_assets)')
    sp = ap.add_subparsers(dest='cmd', required=True)
    p = sp.add_parser('decode')
    p.add_argument('input')
    p.add_argument('-o', '--output', help='write full JSON (without: print summary)')
    p.add_argument('--raw', help='also write decompressed raw bytes')
    p.set_defaults(func=cmd_decode)
    p = sp.add_parser('build')
    p.add_argument('input')
    p.add_argument('-o', '--output', required=True)
    p.set_defaults(func=cmd_build)
    p = sp.add_parser('convert')
    p.add_argument('input')
    p.add_argument('-o', '--output', required=True)
    p.set_defaults(func=cmd_convert)
    a = ap.parse_args()
    a.func(a)


if __name__ == '__main__':
    main()
