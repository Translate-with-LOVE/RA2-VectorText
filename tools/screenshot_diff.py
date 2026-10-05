#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
screenshot_diff.py -- M1.4 verification helper.

Compares two screenshots of the same game screen taken with Mode=observe and
Mode=draw, and answers the acceptance question: "are the differences confined
to glyph ink, or did anything move?"

usage:
  python screenshot_diff.py before.png after.png [--tol 0] [--out diff.png]
                            [--crop x,y,w,h] [--regions] [--top 12]

what it prints:
  * size / pixel-format check (mismatched sizes are cropped to the overlap)
  * how many pixels differ, and the largest per-channel delta
  * optionally the differing regions as bounding boxes (coarse grid), which is
    what tells you whether text moved or only changed shape
  * a visualisation: identical pixels dimmed, differences in red/yellow
"""
import argparse
import sys

try:
    from PIL import Image, ImageChops
except ImportError:
    print('Pillow is required (bundled with this harness)')
    sys.exit(2)


def load(path):
    im = Image.open(path).convert('RGB')
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('before')
    ap.add_argument('after')
    ap.add_argument('--tol', type=int, default=0,
                    help='per-channel tolerance; 0 = pixel exact')
    ap.add_argument('--out', default=None, help='write a difference image here')
    ap.add_argument('--crop', default=None, help='x,y,w,h applied to both images')
    ap.add_argument('--regions', action='store_true',
                    help='report bounding boxes of differing areas (16x16 blocks)')
    ap.add_argument('--top', type=int, default=10, help='how many regions to print')
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')
    except Exception:
        pass

    a, b = load(args.before), load(args.after)
    if a.size != b.size:
        w = min(a.width, b.width)
        h = min(a.height, b.height)
        print('[!] sizes differ: %s vs %s -- cropping to %dx%d' % (a.size, b.size, w, h))
        a = a.crop((0, 0, w, h))
        b = b.crop((0, 0, w, h))

    if args.crop:
        x, y, w, h = (int(v) for v in args.crop.split(','))
        a = a.crop((x, y, x + w, y + h))
        b = b.crop((x, y, x + w, y + h))

    diff = ImageChops.difference(a, b)
    if args.tol:
        # clamp small differences to zero
        diff = diff.point(lambda v: 0 if v <= args.tol else v)

    total = a.width * a.height
    bbox = diff.getbbox()
    if bbox is None:
        print('[=] images are identical (tol=%d) -- %d pixels' % (args.tol, total))
        return 0

    # statistics
    hist_max = 0
    differing = 0
    gray = diff.convert('L')
    px = gray.load()
    for yy in range(gray.height):
        for xx in range(gray.width):
            v = px[xx, yy]
            if v:
                differing += 1
                if v > hist_max:
                    hist_max = v

    print('[*] image      : %dx%d (%d px)' % (a.width, a.height, total))
    print('[*] differing  : %d px (%.3f%%)' % (differing, 100.0 * differing / total))
    print('[*] max delta  : %d / 255' % hist_max)
    print('[*] diff bbox  : %s' % (bbox,))

    regions = []
    if args.regions:
        BS = 16
        for by in range(0, a.height, BS):
            for bx in range(0, a.width, BS):
                box = (bx, by, min(bx + BS, a.width), min(by + BS, a.height))
                sub = gray.crop(box)
                if sub.getbbox() is not None:
                    n = sum(sub.histogram()[1:])
                    regions.append((n, box))
        regions.sort(reverse=True)
        merged = []
        for n, box in regions:
            # merge into an existing region when they touch
            for i, (m, mb) in enumerate(merged):
                if not (box[2] < mb[0] - BS or box[0] > mb[2] + BS or
                        box[3] < mb[1] - BS or box[1] > mb[3] + BS):
                    merged[i] = (m + n, (min(mb[0], box[0]), min(mb[1], box[1]),
                                         max(mb[2], box[2]), max(mb[3], box[3])))
                    break
            else:
                merged.append((n, box))
        merged.sort(reverse=True)
        print('[*] regions   : %d differing area(s)' % len(merged))
        for n, box in merged[:args.top]:
            print('      %6d px  x=%d..%d y=%d..%d' % (n, box[0], box[2], box[1], box[3]))

    if args.out:
        vis = Image.new('RGB', a.size, (0, 0, 0))
        va, vb, vv = a.load(), b.load(), vis.load()
        gpx = gray.load()
        for yy in range(a.height):
            for xx in range(a.width):
                if gpx[xx, yy]:
                    # red = changed, brighter = bigger change
                    v = gpx[xx, yy]
                    vv[xx, yy] = (255, 255 - v, 0)
                else:
                    r, g, bl = va[xx, yy]
                    vv[xx, yy] = (r // 4, g // 4, bl // 4)
        vis.save(args.out)
        print('[*] wrote     : %s' % args.out)

    return 0


if __name__ == '__main__':
    sys.exit(main())
