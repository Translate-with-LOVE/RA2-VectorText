#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
analyze_log.py -- turn a VectorText.log into the data we actually need:

  * per-hook call counts and distinct-string counts
  * per-call-site ("caller") clusters, with sample strings -> the UI context list
  * for the hooks whose argument layout is still a guess: at which stack offset
    the real string sits, per call site  -> resolves the true signature
  * a plausibility filter that flags false positives of the string heuristic
  * top strings by draw count (what a vector renderer must cover first)

usage:  python analyze_log.py [path\\to\\VectorText.log] [--top N] [--md out.md]
"""
import argparse
import os
import re
import sys
from collections import Counter, defaultdict

# the log is UTF-8 with CJK text; make sure printing survives any console code page
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')
except Exception:
    pass

NEW_RE = re.compile(r'^\[\s*(\d+) ms\] NEW\s+(\S+)\s+caller=(0x[0-9a-fA-F]+) text="((?:[^"\\]|\\.)*)"(?:\s+(.*))?$')
MISS_RE = re.compile(r'^\[\s*\d+ ms\] MISS\s+(\S+)\s+caller=(0x[0-9a-fA-F]+)\s*(.*)$')
UNIQ_RE = re.compile(r'^UNIQ\s+(\S+)\s+x(\d+)\s+first=(0x[0-9a-fA-F]+)\s+"((?:[^"\\]|\\.)*)"(?:\s+(.*))?$')
STATS_RE = re.compile(r'^\[\s*\d+ ms\] (STATS|FINAL)\s+(.*)$')
OFF_RE = re.compile(r'text@esp\+(0x[0-9a-fA-F]+)')
ARG_RE = re.compile(r'esp\+0x([0-9A-Fa-f]+)=(0x[0-9a-fA-F]{8})')

DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'VectorText.log')


def unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'x' and i + 3 < len(s):
                try:
                    out.append(chr(int(s[i + 2:i + 4], 16)))
                    i += 4
                    continue
                except ValueError:
                    pass
            out.append(n)
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def plausible(text):
    """A string we are willing to call real UI text."""
    if len(text) < 2:
        return False
    good = 0
    for ch in text:
        o = ord(ch)
        if 0x20 <= o < 0x7F or 0x4E00 <= o <= 0x9FFF or 0x3000 <= o <= 0x303F or 0xFF00 <= o <= 0xFFEF:
            good += 1
    return good * 10 >= len(text) * 9


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path', nargs='?', default=DEFAULT)
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--md', help='also write a markdown report here')
    args = ap.parse_args()

    path = os.path.abspath(args.path)
    if not os.path.exists(path):
        print('[x] not found: %s' % path)
        return 1

    header = []
    proc = exe = ''
    final_counts = {}
    last_stats = {}
    uniq = defaultdict(list)              # hook -> [(count, caller, text, extra)]
    misses = defaultdict(list)            # (hook, caller) -> [dump, ...]
    callsites = defaultdict(lambda: defaultdict(lambda: {'n': 0, 'good': 0, 'bad': 0,
                                                         'offsets': Counter(), 'samples': [],
                                                         'surfaces': Counter(), 'fonts': Counter(),
                                                         'xy': []}))
    off_by_hook = defaultdict(Counter)
    total_new = 0
    garbage = []

    with open(path, 'r', encoding='utf-8-sig', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if line.startswith('PROC'):
                proc = line
            elif line.startswith('EXE'):
                exe = line
            elif line.startswith('==') or line.startswith(' VectorText') or line.startswith(' log=') or line.startswith(' hooks:'):
                header.append(line)

            m = NEW_RE.match(line)
            if m:
                total_new += 1
                hook, caller, text, extra = m.group(2), m.group(3).lower(), unescape(m.group(4)), m.group(5) or ''
                site = callsites[hook][caller]
                site['n'] += 1
                if plausible(text):
                    site['good'] += 1
                    if len(site['samples']) < 3:
                        site['samples'].append(text)
                else:
                    site['bad'] += 1
                    if len(garbage) < 20:
                        garbage.append((hook, caller, text, extra))
                mo = OFF_RE.search(extra)
                if mo:
                    site['offsets'][int(mo.group(1), 16)] += 1
                    off_by_hook[hook][int(mo.group(1), 16)] += 1

                # object arguments, per the layouts confirmed by run #2:
                #   BitText::Print / DrawText : esp+4 = BitFont*, esp+8 = Surface*,
                #                               esp+0x10 = X, esp+0x14 = Y
                args = dict((int(k, 16), v.lower()) for k, v in ARG_RE.findall(extra))
                if 'BitText::' in hook:
                    if 4 in args:
                        site['fonts'][args[4]] += 1
                    if 8 in args:
                        site['surfaces'][args[8]] += 1
                    if 0x10 in args and 0x14 in args and len(site['xy']) < 3:
                        site['xy'].append('%s,%s' % (args[0x10], args[0x14]))
                continue

            m = MISS_RE.match(line)
            if m:
                misses[(m.group(1), m.group(2).lower())].append(m.group(3))
                continue

            m = UNIQ_RE.match(line)
            if m:
                uniq[m.group(1)].append((int(m.group(2)), m.group(3).lower(), unescape(m.group(4)), m.group(5) or ''))
                continue

            m = STATS_RE.match(line)
            if m:
                kind, rest = m.group(1), m.group(2)
                pairs = dict(re.findall(r'(\S+?)=(\d+)', rest))
                last_stats = pairs
                if kind == 'FINAL':
                    final_counts = pairs

    hooks = sorted(set(list(uniq.keys()) + list(callsites.keys()) + list(final_counts.keys())))

    out = []
    def w(s=''):
        out.append(s)
        print(s)

    w('# VectorText M0 log analysis')
    w()
    w('log      : %s' % path)
    if proc:
        w('process  : %s' % proc)
    if exe:
        w('exe      : %s' % exe)
    w('NEW lines: %d ; distinct strings: %d' % (total_new, sum(len(v) for v in uniq.values())))
    w()
    w('## per hook')
    w()
    w('| hook | calls (final) | distinct strings | call sites | plausible NEW | false positives |')
    w('|---|---|---|---|---|---|')
    for h in hooks:
        sites = callsites.get(h, {})
        good = sum(s['good'] for s in sites.values())
        bad = sum(s['bad'] for s in sites.values())
        w('| `%s` | %s | %d | %d | %d | %d |' % (
            h, final_counts.get(h, '?'), len(uniq.get(h, [])), len(sites), good, bad))
    w()
    w('## string argument offset (resolves the real signature)')
    w()
    for h in hooks:
        if off_by_hook.get(h):
            w('* `%s`: %s' % (h, ', '.join('esp+0x%X x%d' % (o, n) for o, n in sorted(off_by_hook[h].items()))))
    w()
    w('## call sites (UI contexts)')
    w()
    for h in hooks:
        sites = callsites.get(h, {})
        if not sites:
            continue
        w('### %s' % h)
        w()
        w('| caller | NEW | plausible | offsets | surfaces (target) | BitFont | X,Y samples | sample strings |')
        w('|---|---|---|---|---|---|---|---|')
        for caller, s in sorted(sites.items(), key=lambda kv: -kv[1]['n']):
            offs = ', '.join('0x%X(%d)' % (o, n) for o, n in s['offsets'].most_common(3)) or '-'
            surfs = ', '.join('%s x%d' % (k, v) for k, v in s['surfaces'].most_common(3)) or '-'
            fonts = ', '.join('%s x%d' % (k, v) for k, v in s['fonts'].most_common(2)) or '-'
            xy = ' / '.join(s['xy']) or '-'
            samples = ' / '.join(t.replace('|', '\\|')[:38] for t in s['samples']) or '-'
            w('| 0x%s | %d | %d | %s | %s | %s | %s | %s |' % (caller[2:], s['n'], s['good'],
                                                              offs, surfs, fonts, xy, samples))
        w()

    w('## top strings by draw count')
    w()
    all_uniq = []
    for h, items in uniq.items():
        for c, caller, text, extra in items:
            all_uniq.append((c, h, caller, text))
    all_uniq.sort(reverse=True)
    w('| x | hook | first caller | text |')
    w('|---|---|---|---|')
    for c, h, caller, text in all_uniq[:args.top]:
        w('| %d | `%s` | 0x%s | %s |' % (c, h, caller[2:], text.replace('|', '\\|')[:70]))

    if misses:
        w()
        w('## calls with no readable string (MISS) -- signature evidence')
        w()
        for (h, caller), dumps in sorted(misses.items(), key=lambda kv: kv[0]):
            w('* `%s` caller=0x%s  x%d logged' % (h, caller[2:], len(dumps)))
            for d in dumps[:2]:
                w('  * %s' % d.strip()[:220])

    if garbage:
        w()
        w('## false positives of the string heuristic (tune LooksLikeText with these)')
        w()
        for h, caller, text, extra in garbage[:10]:
            w('* `%s` caller=0x%s text=%r  %s' % (h, caller[2:], text, extra[:100]))

    if args.md:
        with open(args.md, 'w', encoding='utf-8') as f:
            f.write('\n'.join(out) + '\n')
        print('\n[ok] report written to %s' % args.md)
    return 0


if __name__ == '__main__':
    sys.exit(main())
