#!/usr/bin/env python3
"""Extract the editor's value display tables ("Codeing": "ValToStr_NNN") from the decompiled Hotone app.

The app formats parameter values in HTModelManagerCenter.getControlsConversionValue(), a long if/else chain:
    if (codeing == "ValToStr_053") return ["Raw", "Thin", "Fat"][value]; ...
    if (codeing == "ValToStr_025" || codeing == "ValToStr_040" || ...) return "$value ms"; ...
blutter's disassembly keeps the string constants in code order, so every string after a key belongs to it;
keys without strings of their own (part of an "||" group) get the strings of the next key with strings.

Input:  blutter output, asm/mp350/model/HTModelManager.dart (android/blutter_out/... - not in git)
Output: tools/data/valtostr.json  {"ValToStr_053": ["Raw", "Thin", "Fat"], ...} (raw, not yet interpreted)

Usage: extract_valtostr.py <HTModelManager.dart> [-o tools/data/valtostr.json]
"""
import argparse
import json
import re

FUNC = 'getControlsConversionValue('


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dart')
    ap.add_argument('-o', '--out', default='tools/data/valtostr.json')
    a = ap.parse_args()
    lines = open(a.dart, encoding='utf-8', errors='replace').read().splitlines()
    start = next(i for i, l in enumerate(lines) if FUNC in l and not l.startswith('    '))
    end = next(i for i in range(start + 1, len(lines)) if lines[i] == '  }')
    own, order = {}, []
    cur = None
    for l in lines[start:end]:
        m = re.search(r'// 0x[0-9a-f]+: r\d+ = "(.*)"$', l)
        if not m:
            continue
        s = m.group(1)
        if re.fullmatch(r'ValToStr_\d+', s):
            cur = s
            if cur not in own:
                own[cur] = []
                order.append(cur)
        elif cur:
            own[cur].append(s)
    table, run = {}, []
    for k in order:  # "||" groups share the strings of their last key
        run.append(k)
        if own[k]:
            for r in run:
                table[r] = own[k]
            run = []
    for r in run:
        table[r] = []
    with open(a.out, 'w', encoding='utf-8') as f:
        json.dump(dict(sorted(table.items())), f, ensure_ascii=False, indent=1)
    print(f'{len(table)} codings -> {a.out}')


if __name__ == '__main__':
    main()
