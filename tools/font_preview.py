#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
font_preview.py -- offline comparison sheet for the M1 font decision.

Top row of each pair  : the game's own game.fnt bitmap glyphs (the current look)
Bottom row of each pair: the same string rasterised from a vector font via
                         Pillow (which uses FreeType internally)

usage:
  python font_preview.py --ttf C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf --size 17 \
         --out preview.png [--strings "..." "..."]
"""
import argparse
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

DEFAULT_FNT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'game.fnt')
DEFAULT_TTF = r'C:\Windows\Fonts\NotoSerifSC-VF.ttf'

DEFAULT_STRINGS = [
    "军事行动： 风暴使者 - 地点： 维尔京群岛",
    "距离敌军到达时间：  04:12",
    "难度： 普通    电力=2160  负载=450",
    "选取部队横越画面  提示 - 猎杀无人机会自动搜寻敌人",
    "Phobos development build #48. Please test.",
]


class GameFnt:
    """reader for the RA2 'fonT' bitmap font (see fnt-inspect.py)"""

    def __init__(self, path):
        d = open(path, 'rb').read()
        magic, self.ideograph_width, self.stride, self.lines, self.font_height, \
            self.count, self.symbol_size = struct.unpack_from('<7I', d, 0)
        assert magic == 0x546E6F66, 'not a fonT file'
        self.table = struct.unpack_from('<65536H', d, 0x1C)
        self.glyph_off = 0x1C + 0x20000
        self.data = d

    def glyph(self, ch):
        cp = ord(ch)
        if cp >= 0x10000 or not self.table[cp]:
            return None
        idx = self.table[cp] - 1
        o = self.glyph_off + idx * self.symbol_size
        return self.data[o], self.data[o + 1:o + self.symbol_size]      # width, rows

    def draw_string(self, img, x, y, text, color=0):
        px = img.load()
        for ch in text:
            g = self.glyph(ch)
            if g is None:
                x += self.stride * 8 // 2
                continue
            width, rows = g
            for r in range(self.lines):
                row = rows[r * self.stride:(r + 1) * self.stride]
                bits = int.from_bytes(row, 'big')
                nbits = self.stride * 8
                for c in range(min(width, nbits)):
                    if (bits >> (nbits - 1 - c)) & 1:
                        if 0 <= x + c < img.width and 0 <= y + r < img.height:
                            px[x + c, y + r] = color
            x += width
        return x


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fnt', default=DEFAULT_FNT)
    ap.add_argument('--ttf', default=DEFAULT_TTF)
    ap.add_argument('--size', type=int, default=17)
    ap.add_argument('--out', default='font-preview.png')
    ap.add_argument('--strings', nargs='*', default=DEFAULT_STRINGS)
    ap.add_argument('--measure', action='store_true',
                    help='print game.fnt vs vector advance widths for sizes 12..20 instead of drawing')
    ap.add_argument('--sizes', default='12,13,14,15,16,17,18,19,20')
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')
    except Exception:
        pass

    fnt = GameFnt(os.path.abspath(args.fnt))

    if args.measure:
        def game_width(s):
            w = 0
            for ch in s:
                g = fnt.glyph(ch)
                w += g[0] if g else fnt.stride * 8 // 2
            return w

        sizes = [int(x) for x in args.sizes.split(',')]
        print('advance width comparison (game.fnt vs Noto at N px)')
        print('%-46s %6s %s' % ('string', 'fnt', ' '.join('%6d' % s for s in sizes)))
        totals = dict((s, 0) for s in sizes)
        fnt_total = 0
        for s in args.strings:
            gw = game_width(s)
            fnt_total += gw
            cells = []
            for sz in sizes:
                f = ImageFont.truetype(args.ttf, sz)
                v = f.getlength(s)
                totals[sz] += v
                cells.append('%6.2f' % (v / gw if gw else 0))
            print('%-46s %6d %s' % (s[:44], gw, ' '.join(cells)))
        print('%-46s %6d %s' % ('TOTAL (ratio vs game.fnt)', fnt_total,
                                ' '.join('%6.2f' % (totals[sz] / fnt_total) for sz in sizes)))
        print('\n1.00 = same total advance as the original bitmap font '
              '(what Metrics=game needs to keep every UI box unchanged)')
        return

    ttf = ImageFont.truetype(args.ttf, args.size)
    print('game.fnt  : stride=%d lines=%d height=%d glyphs=%d'
          % (fnt.stride, fnt.lines, fnt.font_height, fnt.count))
    print('vector    : %s @ %dpx (ascent=%d descent=%d)'
          % (os.path.basename(args.ttf), args.size, ttf.getmetrics()[0], ttf.getmetrics()[1]))

    margin, gap, line_gap = 8, 4, 10
    game_h = fnt.lines
    vec_h = args.size + 4
    band_h = game_h + vec_h + gap + line_gap

    W = 1100
    H = margin * 2 + band_h * len(args.strings)
    img = Image.new('L', (W, H), 255)

    y = margin
    for s in args.strings:
        # game.fnt (top) -- note: the real engine draws these at 1:1 in game pixels
        fnt.draw_string(img, margin, y, s, color=0)
        # vector font (bottom)
        draw = ImageDraw.Draw(img)
        draw.text((margin, y + game_h + gap), s, font=ttf, fill=0)
        y += band_h

    img.save(args.out)
    print('written   : %s (%dx%d)  [top = game.fnt, bottom = %s]'
          % (args.out, W, H, os.path.basename(args.ttf)))


if __name__ == '__main__':
    main()
