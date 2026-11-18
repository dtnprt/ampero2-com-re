#!/usr/bin/env python3
"""Build the effect database of the web editor (ampero/data/algorithms.json) from the Hotone editor's data.

Inputs
  * the editor's algorithm databases (flutter_assets/assets/data/algorithm.json and v*_alg_data.json, from
    the Windows editor and/or the Android app - several directories may be given; later versions win)
  * tools/data/valtostr.json - the value display tables (see extract_valtostr.py)

Output (compact JSON):
  {"source": ..., "categories": ["DYN", ...],            # index = Catalog[].Index = tag 4 mod[] value
   "algs": {"<code>": {"x": code in hex, "n": name, "m": module, "g": sub-category (classify, e.g. "Hi Gain"),
                       "c": category index or -1, "p": [param, ...]}}}
  written readable: one algorithm per block, one parameter per line
  param: {"i": Widget.ID, "n": name, "t": type, "min", "max", "s": step, "d": default,
          optional "o": [option names, index = value - min], "u": unit, "off": 1 (min shows "Off"),
          "lim": 1 (max shows "Limit"), "sync": 1 (value is a note index when Sync is on), "k": knobID}
  type: 0 knob, 2 selector, 3 switch, 4 EQ slider (Widget.Type)

The display rules are derived from the app's getControlsConversionValue(): option lists for selectors /
switches are verified against the value range (43 of 45 codings fit); units are a heuristic.

Usage: gen_web_algdb.py <data dir> [<data dir> ...] -o ../dtnprt.github.io/ampero/data/algorithms.json
"""
import argparse
import glob
import json
import os
import re

UNITS = ['Hz', 'kHz', 'dB', 'ms', 's', '%', '°', ':1']


def num(v):
    f = float(v)
    return int(f) if f == int(f) else f


def version_key(path):
    m = re.search(r'v(\d+)\.(\d+)\.(\d+)', os.path.basename(path))
    return (0,) if not m else (1, *map(int, m.groups()), 'after' in path)


def load_algs(dirs):
    files = []
    for d in dirs:
        files += glob.glob(os.path.join(d, 'algorithm.json')) + glob.glob(os.path.join(d, 'v*_alg_data.json'))
    files.sort(key=version_key)
    algs, cats = {}, {}
    for f in files:
        d = json.load(open(f, encoding='utf-8'))
        for c in d['PluginProperties']['Modules']['Catalog']:
            if c.get('Index') is not None:
                cats[int(c['Index'])] = c['Name']
            for a in c['Alg'] if isinstance(c['Alg'], list) else [c['Alg']]:
                w = a.get('Widget') or []
                a = dict(a, Widget=[w] if isinstance(w, dict) else w)
                algs[int(a['Code'])] = (c.get('Index'), c['Name'], a)
    return files, cats, algs


def param(w, table):
    t = int(w['Type'])
    lo, hi = num(w['DisplayMin']), num(w['DisplayMax'])
    p = {'i': int(w['ID']), 'n': w['Name'], 't': t, 'min': lo, 'max': hi, 's': num(w['Step']), 'd': num(w['DefaultValue'])}
    strs = table.get(w.get('Codeing'), [])
    if t in (2, 3) and strs:
        n = int(hi - lo) + 1
        if len(strs) == n:
            p['o'] = strs
        elif len(strs) == int(hi) + 1:  # list indexed by the value itself (min > 0)
            p['o'] = strs[int(lo):]
    elif t in (0, 4) and strs:
        unit = next((u for u in UNITS if u in strs), None)
        if unit:
            p['u'] = unit
        if 'Off' in strs and unit:
            p['off'] = 1
        if 'Limit' in strs:
            p['lim'] = 1
    if w.get('Sync') not in (None, '', '0'):
        p['sync'] = 1
    if w.get('knobID') not in (None, ''):
        p['k'] = int(w['knobID'])
    return p


def pretty(out):
    """Readable JSON: one algorithm per block, one parameter per line (still plain JSON)."""
    j = lambda v: json.dumps(v, ensure_ascii=False)
    lines = ['{', f'  "source": {j(out["source"])},', f'  "categories": {j(out["categories"])},', '  "algs": {']
    items = list(out['algs'].items())
    for k, (code, a) in enumerate(items):
        head = ', '.join(f'{j(key)}: {j(a[key])}' for key in ('x', 'n', 'm', 'g', 'c'))
        lines.append(f'    {j(code)}: {{{head}, "p": [')
        for i, p in enumerate(a['p']):
            lines.append(f'      {j(p)}{"," if i < len(a["p"]) - 1 else ""}')
        lines.append(f'    ]}}{"," if k < len(items) - 1 else ""}')
    lines += ['  }', '}', '']
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--valtostr', default=os.path.join(os.path.dirname(__file__), 'data', 'valtostr.json'))
    a = ap.parse_args()
    table = json.load(open(a.valtostr, encoding='utf-8'))
    files, cats, algs = load_algs(a.dirs)
    out = {
        'source': 'Hotone editor data: ' + ', '.join(sorted({os.path.basename(f) for f in files})),
        'categories': [cats.get(i, '?') for i in range(max(cats) + 1)],
        'algs': {},
    }
    for code in sorted(algs):
        idx, mod, al = algs[code]
        out['algs'][str(code)] = {'x': f'0x{code:08x}', 'n': al['Name'], 'm': mod, 'g': al.get('classify', ''),
                                  'c': int(idx) if idx is not None else -1,
                                  'p': sorted((param(w, table) for w in al['Widget']), key=lambda p: p['i'])}
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8') as f:
        f.write(pretty(out))
    n = sum(len(v['p']) for v in out['algs'].values())
    print(f'{len(out["algs"])} algorithms, {n} parameters -> {a.out} ({os.path.getsize(a.out) // 1024} KB)')


if __name__ == '__main__':
    main()
