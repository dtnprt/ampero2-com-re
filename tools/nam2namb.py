#!/usr/bin/env python3
"""Convert a Neural Amp Modeler model (.nam, JSON) to the compact binary .namb format the Ampero loads.

Port of tools/nam2namb.cpp from TONE3000's nam-binary-loader (MIT, https://github.com/tone-3000/nam-binary-loader,
commit 92c9e85 "Support SlimmableContainer and new WaveNet head format"). The Hotone editor uses that converter
(HTUSBTools.dll: convertNamToNambWithSlim, log "SlimmableContainer: slim=0 -> submodel[0]") - this port produces
byte-identical output (verified against captures/wine-upload-nam-ir-2026-11-17.txt).

Layout (little-endian):
  header   32 B: u32 'NAMB' (0x4E414D42), u16 version 1, u16 flags 0, u32 file size, u32 weights offset,
                 u32 weight count, u32 model block size, u32 CRC32 (zlib, whole file without these 4 bytes), u32 0
  metadata 48 B: u8 version major/minor/patch, u8 flags (1 loudness, 2 input level, 4 output level),
                 f64 sample rate (-1 = unknown), f64 loudness, f64 input dBu, f64 output dBu, 12 zero bytes
  model block:   u8 architecture (0 Linear, 1 ConvNet, 2 LSTM, 3 WaveNet), u8 0, u16 config size, config
  padding to 4, then all weights as f32

Usage: nam2namb.py [--slim 0.0] input.nam [output.namb]

Based on nam-binary-loader, MIT License, Copyright (c) 2026 TONE3000:
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice
and this permission notice shall be included in all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
"""
import argparse
import json
import struct
import sys
import zlib

ARCH = {'Linear': 0, 'ConvNet': 1, 'LSTM': 2, 'WaveNet': 3}
ACTIVATIONS = ['Tanh', 'Hardtanh', 'Fasttanh', 'ReLU', 'LeakyReLU', 'PReLU', 'Sigmoid', 'SiLU', 'Hardswish',
               'LeakyHardtanh', 'Softsign']  # NeuralAmpModelerCore activations::ActivationType
GATING = {'none': 0, 'gated': 1, 'blended': 2}


class W:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v): self.b += struct.pack('<B', v)
    def u16(self, v): self.b += struct.pack('<H', v)
    def u32(self, v): self.b += struct.pack('<I', v)
    def i32(self, v): self.b += struct.pack('<i', v)
    def f32(self, v): self.b += struct.pack('<f', v)
    def f64(self, v): self.b += struct.pack('<d', v)
    def zeros(self, n): self.b += bytes(n)
    def pos(self): return len(self.b)


def activation(w, a):
    if isinstance(a, str):
        a = {'type': a}
    t = a['type']
    t = {'Swish': 'SiLU'}.get(t, t)
    if t not in ACTIVATIONS:
        raise ValueError(f'unknown activation {t!r}')
    params = []
    if t == 'LeakyReLU' and a.get('negative_slope') is not None:
        params = [a['negative_slope']]
    elif t == 'PReLU':
        params = a.get('negative_slopes') or ([a['negative_slope']] if a.get('negative_slope') is not None else [])
    elif t == 'LeakyHardtanh':
        params = [a.get('min_val', -1.0), a.get('max_val', 1.0), a.get('min_slope', 0.01), a.get('max_slope', 0.01)]
    w.u8(ACTIVATIONS.index(t))
    w.u8(len(params))
    for p in params:
        w.f32(p)


def film(w, layer, key):
    f = layer.get(key)
    if not f:  # missing or false
        w.u8(0), w.u8(0), w.u16(1)
        return
    flags = (1 if f.get('active', True) else 0) | (2 if f.get('shift', True) else 0)
    w.u8(flags), w.u8(0), w.u16(f.get('groups', 1))


def metadata(w, m):
    major, minor, patch = (int(x) for x in (m['version'].split('.') + ['0', '0'])[:3])
    w.u8(major), w.u8(minor), w.u8(patch)
    meta = m.get('metadata') or {}
    flags, vals = 0, []
    for bit, key in ((1, 'loudness'), (2, 'input_level_dbu'), (4, 'output_level_dbu')):
        if meta.get(key) is not None:
            flags |= bit
            vals.append(float(meta[key]))
        else:
            vals.append(0.0)
    w.u8(flags)
    w.f64(float(m['sample_rate']) if 'sample_rate' in m else -1.0)
    for v in vals:
        w.f64(v)
    w.zeros(12)


def collect_weights(m, out):
    if m['architecture'] == 'WaveNet' and 'condition_dsp' in m['config']:
        collect_weights(m['config']['condition_dsp'], out)
    out.extend(m.get('weights', []))


def wavenet(w, m):
    c = m['config']
    head = c.get('head') is not None
    if head:
        print("warning: model has a top-level 'head' config (not supported by NAM core)", file=sys.stderr)
    w.u8(c.get('in_channels', 1)), w.u8(1 if head else 0), w.u8(len(c['layers'])), w.u8(1 if 'condition_dsp' in c else 0)
    if 'condition_dsp' in c:
        cw = []
        collect_weights(c['condition_dsp'], cw)
        w.u32(len(cw))
        metadata(w, c['condition_dsp'])
        model_block(w, c['condition_dsp'])
    for L in c['layers']:
        ch = L['channels']
        dil = L['dilations']
        n = len(dil)
        ks = L['kernel_sizes'] if isinstance(L.get('kernel_sizes'), list) else [L['kernel_size']] * n
        if len(ks) != n:
            raise ValueError('kernel_sizes length does not match dilations length')
        if L.get('head') is not None:
            hs, hk, hb = L['head']['out_channels'], L['head']['kernel_size'], L['head']['bias']
        else:
            hs, hk, hb = L['head_size'], 1, L['head_bias']
        for v in (L['input_size'], L['condition_size'], hs, ch, L.get('bottleneck', ch), hk):
            w.u16(v)
        w.u8(1 if hb else 0), w.u8(n)
        w.u16(L.get('groups_input', 1)), w.u16(L.get('groups_input_mixin', 1))
        l1 = L.get('layer1x1', {'active': True, 'groups': 1})
        w.u8(1 if l1['active'] else 0), w.u16(l1['groups']), w.u8(0)
        h1 = L.get('head1x1', {'active': False, 'out_channels': ch, 'groups': 1})
        w.u8(1 if h1['active'] else 0), w.u16(h1['out_channels']), w.u16(h1['groups']), w.u8(0)
        for key in ('conv_pre_film', 'conv_post_film', 'input_mixin_pre_film', 'input_mixin_post_film',
                    'activation_pre_film', 'activation_post_film', 'layer1x1_post_film', 'head1x1_post_film'):
            film(w, L, key)
        for d in dil:
            w.i32(d)
        for k in ks:
            w.u16(k)
        acts = L['activation'] if isinstance(L['activation'], list) else [L['activation']] * n
        for a in acts:
            activation(w, a)
        if 'gating_mode' in L:
            gm = L['gating_mode'] if isinstance(L['gating_mode'], list) else [L['gating_mode']] * n
            gm = [GATING[x] for x in gm]
        elif 'gated' in L:
            gm = [1 if L['gated'] else 0] * n
        else:
            gm = [0] * n
        for g in gm:
            w.u8(g)
        sec = L.get('secondary_activation')
        for i, g in enumerate(gm):
            if g:
                if sec is None:
                    w.u8(ACTIVATIONS.index('Sigmoid')), w.u8(0)
                else:
                    activation(w, sec[i] if isinstance(sec, list) else sec)
            else:
                w.u8(ACTIVATIONS.index('Tanh')), w.u8(0)  # placeholder, type does not matter


def model_block(w, m):
    arch = ARCH[m['architecture']]
    c = m['config']
    w.u8(arch), w.u8(0)
    size_pos = w.pos()
    w.u16(0)
    start = w.pos()
    if arch == 0:
        w.i32(c['receptive_field']), w.u8(1 if c['bias'] else 0), w.u8(c.get('in_channels', 1)), w.u8(c.get('out_channels', 1)), w.u8(0)
    elif arch == 1:
        w.u16(c['channels']), w.u8(1 if c['batchnorm'] else 0), w.u8(len(c['dilations'])), w.u16(c.get('groups', 1))
        w.u8(c.get('in_channels', 1)), w.u8(c.get('out_channels', 1))
        activation(w, c['activation'])
        for d in c['dilations']:
            w.i32(d)
    elif arch == 2:
        w.u16(c['num_layers']), w.u16(c['input_size']), w.u16(c['hidden_size'])
        w.u8(c.get('in_channels', 1)), w.u8(c.get('out_channels', 1)), w.u16(0)
    else:
        wavenet(w, m)
    size = w.pos() - start
    if size > 65535:
        raise ValueError('config too large')
    w.b[size_pos:size_pos + 2] = struct.pack('<H', size)


def resolve_slimmable(m, slim):
    subs = m['config']['submodels']
    idx = next((i for i, s in enumerate(subs) if slim < s['max_value']), len(subs) - 1)
    sel = dict(subs[idx]['model'])
    for key in ('version', 'metadata', 'sample_rate'):
        if key in m and key not in sel:
            sel[key] = m[key]
    print(f'SlimmableContainer: slim={slim:g} -> submodel[{idx}] (max_value={subs[idx]["max_value"]:g})', file=sys.stderr)
    return sel


def convert(m, slim=0.0):
    """.nam JSON (dict) -> .namb bytes. A SlimmableContainer (NAM A2) is reduced to one submodel first
    (slim 0.0 = the smallest, like the Hotone editor)."""
    if m['architecture'] == 'SlimmableContainer':
        m = resolve_slimmable(m, slim)
    w = W()
    w.u32(0x4E414D42), w.u16(1), w.u16(0)
    w.zeros(24)  # file size, weights offset, weight count, model block size, CRC, reserved: patched below
    metadata(w, m)
    start = w.pos()
    model_block(w, m)
    block = w.pos() - start
    while w.pos() % 4:
        w.u8(0)
    woff = w.pos()
    weights = []
    collect_weights(m, weights)
    w.b += struct.pack(f'<{len(weights)}f', *weights)
    struct.pack_into('<IIII', w.b, 8, len(w.b), woff, len(weights), block)
    struct.pack_into('<I', w.b, 24, zlib.crc32(bytes(w.b[:24]) + bytes(w.b[28:])))
    return bytes(w.b)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--slim', type=float, default=0.0)
    ap.add_argument('input')
    ap.add_argument('output', nargs='?')
    a = ap.parse_args()
    data = convert(json.load(open(a.input, encoding='utf-8')), a.slim)
    out = a.output or a.input.rsplit('.', 1)[0] + '.namb'
    open(out, 'wb').write(data)
    print(f'{a.input} -> {out}: {len(data)} bytes')


if __name__ == '__main__':
    main()
