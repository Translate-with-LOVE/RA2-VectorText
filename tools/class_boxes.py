#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Measure the original game.fnt ink boxes per character class (for aligning
our vector glyphs to the same visual position the bitmap font used)."""
import struct
import sys

FNT = r'F:\Mental Omega\game.fnt'

d = open(FNT, 'rb').read()
magic, ideo, stride, lines, height, count, symsize = struct.unpack_from('<7I', d, 0)
tab = struct.unpack_from('<65536H', d, 0x1C)
off = 0x1C + 0x20000


def box(cp):
    if cp >= 0x10000 or not tab[cp]:
        return None
    g = d[off + (tab[cp] - 1) * symsize: off + tab[cp] * symsize]
    rows, cols = [], []
    for y in range(lines):
        for x in range(stride * 8):
            if g[1 + y * stride + (x >> 3)] & (0x80 >> (x & 7)):
                rows.append(y)
                cols.append(x)
    if not rows:
        return None
    return (min(rows), max(rows), min(cols), max(cols), g[0])


CLASSES = [
    ('ASCII upper',  [ord(c) for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ']),
    ('ASCII lower',  [ord(c) for c in 'abcdefghijklmnopqrstuvwxyz']),
    ('ASCII digits', [ord(c) for c in '0123456789']),
    ('ASCII punct',  [ord(c) for c in '.,:;!?\'"()[]']),
    ('CJK punct',    list(range(0x3001, 0x3012))),
    ('CJK ideograph', list(range(0x4E00, 0x4E00 + 200))),
    ('fullwidth',    list(range(0xFF01, 0xFF5F))),
    ('space',        [0x20]),
]

for name, cps in CLASSES:
    rows, cols, adv = [], [], []
    for cp in cps:
        b = box(cp)
        if b:
            rows.append((b[0], b[1]))
            cols.append((b[2], b[3]))
            adv.append(b[4])
    if rows:
        print('%-14s rows %2d..%2d   cols %2d..%2d   advance %d..%d  (n=%d)'
              % (name, min(r[0] for r in rows), max(r[1] for r in rows),
                 min(c[0] for c in cols), max(c[1] for c in cols),
                 min(adv), max(adv), len(rows)))
    else:
        print('%-14s (no glyphs present)' % name)
