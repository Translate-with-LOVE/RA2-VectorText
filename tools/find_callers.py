#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 VectorText contributors
# SPDX-License-Identifier: GPL-3.0-only
# -*- coding: utf-8 -*-
"""
find_callers.py -- static call-site scan for gamemd.exe.

Finds every relative `call rel32` (E8 xx xx xx xx) that targets one of the
given addresses, then prints the call sites grouped by function.  This is how
we answer questions like "who draws glyphs?" (callers of BitFont::Blit) or
"who sets the shadow colour?" (callers of BitFont::SetColor2) without a
debugger.

usage:
  python find_callers.py [--exe gamemd.exe] 0x434120 0x433C80 ...
  python find_callers.py --list              # the addresses this project cares about
"""
import argparse
import os
import struct
import sys
from pathlib import Path

GAME_DEFAULT = str(Path(__file__).resolve().parents[2] / 'gamemd.exe')

# addresses that matter for the text pipeline (see docs/rendering.md)
KNOWN = {
    0x434120: 'BitFont::Blit            (draw one glyph)',
    0x434500: 'BitFont::DrawString      (string loop)',
    0x434B90: 'BitText::Print',
    0x434CD0: 'BitText::DrawText',
    0x4348F0: 'BitFont::Lock',
    0x434990: 'BitFont::UnLock',
    0x433CF0: 'BitFont::GetTextDimension',
    0x434110: 'BitFont::SetX            (this+0x20)',
    0x433C70: 'BitFont::SetColor        (this+0x24, returns old)',
    0x433C80: 'BitFont::SetColor2       (this+0x26, returns colour)',
    0x433C90: 'BitFont::SetFlag         (this+0x41)',
    0x433CA0: 'BitFont::SetBounds       (this+0x30..0x3C)',
    0x4346C0: 'BitFont::GetCharacterBitmap',
    0x434840: 'Rect intersect/clip helper',
    0x4A59E0: 'Drawing::GetTextDimensions',
    0x4A61C0: 'Drawing::PrintUnicode',
}


def scan(data, image_base, target):
    hits = []
    pat = b'\xe8'
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        start = i + 1
        if i + 5 > len(data):
            break
        rel = struct.unpack_from('<i', data, i + 1)[0]
        # the call's next instruction is at i+5 (VA = image_base + i + 5 ... plus section VA)
        # gamemd.exe is a single-section image whose file offset 0x400 maps to VA 0x401000,
        # so VA = image_base + offset for PE images with that alignment; compute properly:
        if image_base + i + 5 + rel == target:
            hits.append(image_base + i)
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('targets', nargs='*', help='hex addresses (0x...)')
    ap.add_argument('--exe', default=GAME_DEFAULT)
    ap.add_argument('--list', action='store_true', help='show the known-address table')
    ap.add_argument('--max', type=int, default=40, help='max call sites printed per target')
    args = ap.parse_args()

    if args.list:
        for addr in sorted(KNOWN):
            print('  0x%08X  %s' % (addr, KNOWN[addr]))
        return 0

    if not args.targets:
        ap.error('give at least one address, or --list')

    if not os.path.exists(args.exe):
        print('[x] not found: %s' % args.exe)
        return 1

    data = open(args.exe, 'rb').read()
    # PE: read ImageBase and the .text section header for the VA<->offset mapping
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    image_base = struct.unpack_from('<I', data, e_lfanew + 0x34)[0]
    nsec = struct.unpack_from('<H', data, e_lfanew + 6)[0]
    opt = struct.unpack_from('<H', data, e_lfanew + 20)[0]
    secs = []
    for i in range(nsec):
        off = e_lfanew + 24 + opt + i * 40
        name = data[off:off + 8].rstrip(b'\0').decode('latin1')
        vsize, va, rawsize, raw = struct.unpack_from('<IIII', data, off + 8)
        secs.append((name, image_base + va, raw, rawsize))
    print('[*] %s  imagebase=0x%08X  sections=%s'
          % (os.path.basename(args.exe), image_base, ','.join(s[0] for s in secs)))

    def va_of(off):
        for name, sva, raw, rawsize in secs:
            if raw <= off < raw + rawsize:
                return sva + (off - raw)
        return None

    for t in args.targets:
        target = int(t, 16)
        hits = []
        # scan the whole file, translating each candidate offset to a VA
        pos = 0
        while True:
            i = data.find(b'\xe8', pos)
            if i < 0 or i + 5 > len(data):
                break
            pos = i + 1
            va = va_of(i)
            if va is None:
                continue
            rel = struct.unpack_from('<i', data, i + 1)[0]
            if va + 5 + rel == target:
                hits.append(va)
        label = KNOWN.get(target, '')
        print('\n[=] callers of 0x%08X  %s  -> %d site(s)'
              % (target, label, len(hits)))
        for h in hits[:args.max]:
            print('      0x%08X' % h)
        if len(hits) > args.max:
            print('      ... %d more' % (len(hits) - args.max))
    return 0


if __name__ == '__main__':
    sys.exit(main())
