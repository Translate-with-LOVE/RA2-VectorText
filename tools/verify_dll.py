#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 VectorText contributors
# SPDX-License-Identifier: GPL-3.0-only
# -*- coding: utf-8 -*-
"""
verify_dll.py -- sanity-check a Syringe hook DLL built by this project.

Checks, without running the game:
  * the file is a 32-bit PE DLL
  * all expected hook handlers are exported with undecorated names
  * a ".syhks00" section exists and holds well-formed 16-byte records
    { u32 hookAddr; u32 hookSize; const char* hookName }
  * every record's name pointer resolves inside the image to the name of an
    exported function (this is how Syringe binds a hook to its handler)
  * every name pointer has a base relocation (needed for ASLR)
  * neither ordinary nor delay imports require Phobos/Ares
  * hook spans do not overlap those of an installed Phobos.dll

usage:  python verify_dll.py [path\\to\\VectorText.dll]
"""
import os
import struct
import sys

DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'VectorText.dll')


def load(path):
    with open(path, 'rb') as f:
        return f.read()


def check_native_raster(path):
    game = os.path.join(os.path.dirname(path), 'gamemd.exe')
    if not os.path.isfile(game):
        return True
    d = load(game)
    nt, = struct.unpack_from('<I', d, 0x3C)
    count, = struct.unpack_from('<H', d, nt + 6)
    optsize, = struct.unpack_from('<H', d, nt + 20)
    timestamp, = struct.unpack_from('<I', d, nt + 8)
    base, = struct.unpack_from('<I', d, nt + 52)
    sections = [struct.unpack_from('<IIII', d, nt + 24 + optsize + i * 40 + 8)
                for i in range(count)]
    def read(va, size):
        for vsize, rva, rawsize, rawptr in sections:
            if rva <= va - base and va - base + size <= rva + rawsize:
                off = rawptr + va - base - rva
                return d[off:off + size]
        return b''
    prefix = bytes.fromhex('81 EC B4 00 00 00 53 56 8B F1 57')
    copy_prefix = bytes.fromhex('8B 44 24 1C 83 EC 20 53 56 8B F1')
    ok = base == 0x400000 and timestamp == 0x3BDF544E and (
        read(0x4BB620, len(prefix)) == prefix and
        read(0x7E85E4, 4) == struct.pack('<I', 0x4BB620) and
        read(0x7E8658, 4) == struct.pack('<I', 0x4C1AB0) and
        read(0x437350, len(copy_prefix)) == copy_prefix and
        read(0x623A97, 8) == bytes.fromhex('89 54 24 30 8D 44 24 30'))
    print('\nnative raster: FillRectEx 0x4BB620 and CPU copy 0x437350, vtable/prefix %s'
          % ('OK' if ok else 'DIFFER -- Present32 will fall back'))
    print('native message background: 0x623A97 setup before Phobos fill %s' %
          ('OK' if read(0x623A97, 8) == bytes.fromhex('89 54 24 30 8D 44 24 30') else 'DIFFER'))
    return ok


def check_phobos_overlap(path, records):
    """Check hook spans against the installed DLL without loading/executing it."""
    phobos = os.path.join(os.path.dirname(path), 'Phobos.dll')
    if not os.path.isfile(phobos):
        return True
    d = load(phobos)
    nt, = struct.unpack_from('<I', d, 0x3C)
    count, = struct.unpack_from('<H', d, nt + 6)
    optsize, = struct.unpack_from('<H', d, nt + 20)
    ok = True
    for i in range(count):
        at = nt + 24 + optsize + i * 40
        if not d[at:at + 8].startswith(b'.syhks'):
            continue
        rawsize, rawptr = struct.unpack_from('<II', d, at + 16)
        for off in range(rawptr, rawptr + rawsize, 16):
            addr, size = struct.unpack_from('<II', d, off)
            if not addr or not size:
                continue
            for ours, length, name in records:
                if ours < addr + size and addr < ours + length:
                    print('Phobos hook overlap: %s 0x%08X/%d with 0x%08X/%d' %
                          (name, ours, length, addr, size))
                    ok = False
    print('installed Phobos hook spans: %s' % ('no overlap' if ok else 'CONFLICT'))
    return ok


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    path = os.path.abspath(path)
    d = load(path)
    print('file      : %s (%d bytes)' % (path, len(d)))

    e_lfanew, = struct.unpack_from('<I', d, 0x3C)
    if d[e_lfanew:e_lfanew + 4] != b'PE\0\0':
        print('[x] not a PE file')
        return 1
    coff = e_lfanew + 4
    machine, nsec = struct.unpack_from('<HH', d, coff)
    optsize, = struct.unpack_from('<H', d, coff + 16)
    characteristics, = struct.unpack_from('<H', d, coff + 18)
    opt = coff + 20
    magic, = struct.unpack_from('<H', d, opt)
    image_base, = struct.unpack_from('<I', d, opt + 28)
    dd = opt + (96 if magic == 0x10B else 112)

    print('machine   : 0x%04X %s' % (machine, 'x86 (ok)' if machine == 0x14C else '<-- NOT 32-bit!'))
    print('image base: 0x%08X' % image_base)
    print('dll flag  : %s' % ('yes' if characteristics & 0x2000 else 'NO'))

    secs = []
    so = opt + optsize
    for i in range(nsec):
        s = so + i * 40
        name = d[s:s + 8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', d, s + 8)
        secs.append((name, vaddr, vsize, rawptr, rawsize))
        print('section   : %-10s VA %08X  vsize %06X  raw %06X  size %06X'
              % (name, image_base + vaddr, vsize, rawptr, rawsize))

    def rva2off(rva):
        for name, va, vsize, rawptr, rawsize in secs:
            if va <= rva < va + max(vsize, rawsize):
                return rawptr + (rva - va)
        return None

    def read_cstr(va):
        off = rva2off(va - image_base)
        if off is None:
            return None
        end = d.index(b'\0', off)
        return d[off:end].decode('latin1')

    # ---- imports: rendering must also work without Phobos/Ares ------------
    imports = []
    for directory, descriptor_size, delayed in ((1, 20, False), (13, 32, True)):
        import_rva, import_size = struct.unpack_from('<II', d, dd + directory * 8)
        if not import_rva:
            continue
        off = rva2off(import_rva)
        end = off + import_size
        while off + descriptor_size <= end:
            fields = struct.unpack_from('<' + 'I' * (descriptor_size // 4), d, off)
            if not any(fields):
                break
            if delayed:
                name_va = image_base + fields[1] if fields[0] & 1 else fields[1]
            else:
                name_va = image_base + fields[3]
            imports.append(read_cstr(name_va))
            off += descriptor_size
    independent = all(name and name.lower() not in ('phobos.dll', 'ares.dll') for name in imports)
    print('\nimports   : %s' % ', '.join(name or '<invalid>' for name in imports))
    print('no Phobos/Ares dependency: %s' % ('ok' if independent else 'BAD'))

    # ---- exports ----------------------------------------------------------
    exp_rva, exp_size = struct.unpack_from('<II', d, dd)
    exports = {}
    if exp_rva:
        o = rva2off(exp_rva)
        (_flags, _ts, _maj, _mnr, name_rva, base, nfunc, nname,
         afunc, aname, aord) = struct.unpack_from('<IIHHIIIIIII', d, o)
        for i in range(nname):
            nr, = struct.unpack_from('<I', d, rva2off(aname) + i * 4)
            orr, = struct.unpack_from('<H', d, rva2off(aord) + i * 2)
            fr, = struct.unpack_from('<I', d, rva2off(afunc) + orr * 4)
            exports[read_cstr(image_base + nr)] = image_base + fr
    print('\nexports   : %d' % len(exports))
    for n in sorted(exports):
        print('   %-40s 0x%08X' % (n, exports[n]))

    # ---- .syhks00 ---------------------------------------------------------
    hooks = [s for s in secs if s[0].startswith('.syhks')]
    if not hooks:
        print('\n[x] no .syhks00 section -- Syringe will not see any hook!')
        return 1

    # relocation target addresses (for the ASLR check)
    reloc_vas = set()
    rel_rva, rel_size = struct.unpack_from('<II', d, dd + 5 * 8)
    if rel_rva:
        o = rva2off(rel_rva)
        end = o + rel_size
        while o < end:
            page, blocksize = struct.unpack_from('<II', d, o)
            if blocksize == 0:
                break
            n = (blocksize - 8) // 2
            for k in range(n):
                ent, = struct.unpack_from('<H', d, o + 8 + k * 2)
                if ent & 0xF000 == 0x3000:                      # IMAGE_REL_BASED_HIGHLOW
                    reloc_vas.add(image_base + page + (ent & 0x0FFF))
            o += blocksize

    print('\n.syhks00 records:')
    ok = independent
    records = []
    for name, va, vsize, rawptr, rawsize in hooks:
        count = 0
        for off in range(rawptr, rawptr + rawsize, 16):
            addr, size, nameptr = struct.unpack_from('<IIQ', d, off)[:3]
            nameptr = nameptr & 0xFFFFFFFF
            if addr == 0 and size == 0 and nameptr == 0:
                continue
            count += 1
            s = read_cstr(nameptr)
            records.append((addr, size, s))
            exported = s in exports if s else False
            # the name pointer field itself must be relocated, otherwise the
            # record breaks as soon as the DLL is loaded at another base
            field_va = image_base + va + (off - rawptr) + 8
            reloc = field_va in reloc_vas
            small = size < 5
            bad = (not exported) or (not reloc) or small
            if bad:
                ok = False
            print('   hookAddr=0x%08X size=%-3d namePtr=0x%08X -> %-40s export=%-5s reloc=%-5s [%s]'
                  % (addr, size, nameptr, repr(s), exported, reloc, 'BAD' if bad else 'ok'))
        print('   (%d records in %s)' % (count, name))

    ok = check_native_raster(path) and ok
    ok = check_phobos_overlap(path, records) and ok
    print('\nresult    : %s' % ('OK -- ready for Syringe' if ok else 'PROBLEMS FOUND'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
