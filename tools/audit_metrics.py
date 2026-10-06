#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
audit_metrics.py -- M2.2 input: which UI text would overflow under natural
(vector) metrics?

Every measurement the game made is in VectorText.log as

    BitFont::GetTextDimension(0x433CF0) caller=0x00478F0B text="..." ... maxW=1752 ...

`maxW` is the width the caller is willing to use, so replaying each recorded
string with (a) the game's own per-character widths and (b) FreeType's natural
widths tells us exactly which call sites would overflow, and by how much.

usage:
  python audit_metrics.py [--log VectorText.log] [--fnt game.fnt]
                          [--latin 13] [--cjk 16] [--weight 700]
                          [--tol 0.0] [--md report.md]
"""
import argparse
import os
import re
import struct
import sys

try:
    from PIL import ImageFont
except ImportError:
    print('Pillow is required')
    sys.exit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LOG = os.path.join(HERE, '..', '..', 'VectorText.log')
DEFAULT_FNT = os.path.join(HERE, '..', '..', 'game.fnt')
DEFAULT_TTF = r'C:\Windows\Fonts\NotoSerifSC-VF.ttf'
CJK_FROM = 0x2E80

LINE_RE = re.compile(r'GetTextDimension\(0x433CF0\).*?caller=0x([0-9A-Fa-f]+)\s+text="((?:[^"\\]|\\.)*)"(.*)$')
MAXW_RE = re.compile(r'maxW=(\d+)')


def unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'n':
                out.append('\n'); i += 2; continue
            if n == 't':
                out.append('\t'); i += 2; continue
            if n == 'r':
                out.append('\r'); i += 2; continue
            if n == 'x' and i + 3 < len(s):
                out.append(chr(int(s[i + 2:i + 4], 16))); i += 4; continue
            out.append(n); i += 2; continue
        out.append(c); i += 1
    return ''.join(out)


class GameFnt:
    def __init__(self, path):
        d = open(path, 'rb').read()
        magic, ideograph, stride, lines, height, count, symsize = struct.unpack_from('<7I', d, 0)
        assert magic == 0x546E6F66, 'not a fonT file'
        self.stride, self.lines, self.count, self.symsize = stride, lines, count, symsize
        self.table = struct.unpack_from('<65536H', d, 0x1C)
        self.data = d
        self.off = 0x1C + 0x20000

    def width(self, ch):
        cp = ord(ch)
        if cp >= 0x10000 or not self.table[cp]:
            return None
        return self.data[self.off + (self.table[cp] - 1) * self.symsize]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', default=DEFAULT_LOG)
    ap.add_argument('--fnt', default=DEFAULT_FNT)
    ap.add_argument('--ttf', default=DEFAULT_TTF)
    ap.add_argument('--latin', type=int, default=13)
    ap.add_argument('--cjk', type=int, default=16)
    ap.add_argument('--weight', type=int, default=700)
    ap.add_argument('--tol', type=float, default=0.0, help='allowed overflow ratio (0.02 = 2%%)')
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--scale', type=float, default=0.0, help='report Metrics=scaled with this advance scale')
    ap.add_argument('--md', default=None)
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')
    except Exception:
        pass

    if not os.path.exists(args.log):
        print('[x] log not found: %s' % args.log)
        return 1

    fnt = GameFnt(os.path.abspath(args.fnt))
    latin = ImageFont.truetype(args.ttf, args.latin)
    cjk = ImageFont.truetype(args.ttf, args.cjk)
    for f in (latin, cjk):
        try:
            f.set_variation_by_axes([args.weight])
        except Exception:
            pass

    def vec_width(ch):
        f = cjk if ord(ch) >= CJK_FROM else latin
        return f.getlength(ch)

    rows = []
    seen = set()
    with open(args.log, 'r', encoding='utf-8-sig', errors='replace') as fh:
        for line in fh:
            m = LINE_RE.search(line)
            if not m:
                continue
            caller, raw, tail = m.group(1), m.group(2), m.group(3)
            mw = MAXW_RE.search(tail)
            if not mw:
                continue
            maxw = int(mw.group(1))
            text = unescape(raw)
            key = (caller.lower(), text, maxw)
            if key in seen or not text:
                continue
            seen.add(key)

            g = 0.0
            v = 0.0
            missing = 0
            for ch in text:
                w = fnt.width(ch)
                if w is None:
                    missing += 1
                    g += 6.0                       # engine placeholder path
                else:
                    g += w
                v += vec_width(ch)
            if args.scale > 0:
                v = sum(round((fnt.width(ch) or 6) * args.scale) for ch in text)
            rows.append(dict(caller=caller.lower(), text=text, maxw=maxw,
                             game=g, vec=v, missing=missing))

    if not rows:
        print('[x] no GetTextDimension measurements with maxW in this log')
        return 3

    over = [r for r in rows if r['vec'] - r['game'] > 1e-6 or r['vec'] > r['maxw']]
    overflowing = [r for r in rows if r['maxw'] > 0 and r['vec'] > r['maxw'] * (1.0 + args.tol)]
    growing = [r for r in rows if r['game'] > 0 and r['vec'] / r['game'] > 1.02]

    print('[*] measurements replayed : %d' % len(rows))
    print('[*] font sizes            : latin %dpx / cjk %dpx, weight %d' % (args.latin, args.cjk, args.weight))
    tot_g = sum(r['game'] for r in rows)
    tot_v = sum(r['vec'] for r in rows)
    print('[*] total width           : game %.0f px  vector %.0f px  ratio %.3f'
          % (tot_g, tot_v, tot_v / tot_g if tot_g else 0))
    print('[*] wider than the game   : %d (%.1f%%)' % (len(growing), 100.0 * len(growing) / len(rows)))
    print('[*] would exceed maxW     : %d (%.1f%%)' % (len(overflowing), 100.0 * len(overflowing) / len(rows)))

    by_site = {}
    for r in overflowing:
        d = by_site.setdefault(r['caller'], [0, 0.0, r])
        d[0] += 1
        d[1] = max(d[1], r['vec'] / r['maxw'] if r['maxw'] else 0)
    print('\n## call sites that would overflow (sorted by worst ratio)')
    if not by_site:
        print('  none')
    for caller, (n, worst, sample) in sorted(by_site.items(), key=lambda kv: -kv[1][1])[:args.top]:
        print('  0x%s  %3d case(s)  worst=%.2fx maxW   sample: "%.40s"' %
              (caller[2:], n, worst, sample['text']))

    print('\n## worst individual cases')
    for r in sorted(overflowing, key=lambda r: -(r['vec'] / r['maxw'] if r['maxw'] else 0))[:args.top]:
        print('  game %6.1f  vector %6.1f  maxW %5d  %+.0f%%  caller 0x%s  "%s"'
              % (r['game'], r['vec'], r['maxw'], 100.0 * (r['vec'] / r['maxw'] - 1.0),
                 r['caller'][2:], r['text'][:38].replace('\n', ' ')))

    if args.md:
        with open(args.md, 'w', encoding='utf-8') as f:
            f.write('# metrics audit\n\n')
            f.write('- measurements: %d, growing: %d, overflowing: %d\n'
                    % (len(rows), len(growing), len(overflowing)))
            f.write('- totals: game %.0f px vs vector %.0f px (%.3f)\n\n'
                    % (tot_g, tot_v, tot_v / tot_g if tot_g else 0))
            f.write('| caller | cases | worst | sample |\n|---|---|---|---|\n')
            for caller, (n, worst, sample) in sorted(by_site.items(), key=lambda kv: -kv[1][1]):
                f.write('| 0x%s | %d | %.2fx | %s |\n'
                        % (caller[2:], n, worst, sample['text'][:40].replace('|', '\\|').replace('\n', ' ')))
        print('\n[ok] report written to %s' % args.md)
    return 0


if __name__ == '__main__':
    sys.exit(main())
