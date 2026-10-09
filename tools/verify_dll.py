#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 VectorText contributors
# SPDX-License-Identifier: GPL-3.0-only
# -*- coding: utf-8 -*-
"""
verify_dll.py -- sanity-check a Syringe hook DLL built by this project.

Checks, without running the game:
  * the file is a 32-bit PE DLL
  * every registered hook names an exported handler
  * a ".syhks00" section exists and holds well-formed 16-byte records
    { u32 hookAddr; u32 hookSize; const char* hookName }
  * every record's name pointer resolves inside the image to the name of an
    exported function (this is how Syringe binds a hook to its handler)
  * every name pointer has a base relocation (needed for ASLR)
  * neither ordinary nor delay imports require Phobos/Ares
  * PE minimum versions and all named imports fit the audited Windows 7 baseline
  * hook spans do not overlap those of an installed Phobos.dll

usage:  python verify_dll.py [path\\to\\VectorText.dll] [path\\to\\game.exe]
"""
import os
import re
import struct
import sys

DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'VectorText.dll')

# APIs used by this project's static MSVC/FreeType/MinHook build. Every entry
# exists in Windows 7 kernel32.dll (including its forwarded exports). Keep this
# positive allowlist: new dependencies must be reviewed instead of silently
# passing an incomplete list of Windows 8/10-only functions. Dynamic rendering
# APIs are checked separately in source; this is a loader/import check only.
WIN7_KERNEL32 = set('''
GetSystemInfo OutputDebugStringA RaiseFailFastException
VirtualQuery DisableThreadLibraryCalls QueryPerformanceCounter QueryPerformanceFrequency
InitializeCriticalSection InitializeCriticalSectionAndSpinCount EnterCriticalSection LeaveCriticalSection VirtualProtect
GetModuleFileNameA GetModuleHandleW GetProcAddress LoadLibraryW CreateFileA GetFileSize
ReadFile SetFilePointer WriteFile CloseHandle GetCurrentProcessId GetTickCount
GetModuleHandleA GetPrivateProfileIntA GetPrivateProfileStringA WideCharToMultiByte
GetCurrentProcess FlushInstructionCache VirtualAlloc GetLastError HeapCreate HeapDestroy
HeapAlloc HeapReAlloc HeapFree Sleep GetCurrentThreadId OpenThread SuspendThread
ResumeThread GetThreadContext SetThreadContext CreateToolhelp32Snapshot Thread32First
Thread32Next VirtualFree DeleteCriticalSection ReleaseSRWLockExclusive AcquireSRWLockExclusive
WakeAllConditionVariable SleepConditionVariableSRW IsProcessorFeaturePresent
GetSystemTimeAsFileTime InitializeSListHead SetUnhandledExceptionFilter GetStartupInfoW
RaiseException RtlUnwind InterlockedFlushSList SetLastError FlsAlloc FlsGetValue FlsSetValue
FlsFree TlsAlloc TlsGetValue TlsSetValue TlsFree EncodePointer InitializeCriticalSectionEx ExitProcess TerminateProcess FreeLibrary
GetModuleHandleExW GetModuleFileNameW IsDebuggerPresent UnhandledExceptionFilter GetStdHandle
GetFileType DecodePointer GetConsoleMode ReadConsoleW SetFilePointerEx FindClose
FindFirstFileExW FindNextFileW IsValidCodePage GetACP GetOEMCP GetCPInfo GetCommandLineA
GetCommandLineW MultiByteToWideChar GetEnvironmentStringsW FreeEnvironmentStringsW
SetEnvironmentVariableW LoadLibraryExW CompareStringW LCMapStringW GetProcessHeap
GetStringTypeW SetStdHandle FlushFileBuffers GetConsoleOutputCP CreateFileW HeapSize
SetEndOfFile WriteConsoleW
'''.split())
# WINDOWINFO is available since Windows 98; its physical client rectangle is
# not cnc-ddraw's virtualized GetClientRect result. No newer DPI API is needed.
WIN7_USER32 = {'GetWindowInfo'}


def check_win7(os_version, subsystem_version, symbols):
    versions_ok = os_version == (6, 1) and subsystem_version == (6, 1)
    allowed = {'kernel32.dll': WIN7_KERNEL32, 'user32.dll': WIN7_USER32}
    unreviewed = [(module, name) for module, name in symbols
                  if name not in allowed.get(module.lower(), set())]
    print('Windows 7 PE target: OS %d.%d, subsystem %d.%d [%s]' %
          (*os_version, *subsystem_version, 'ok' if versions_ok else 'BAD'))
    for module, name in unreviewed:
        print('Windows 7 import needs review: %s!%s' % (module, name))
    print('Windows 7 named import baseline: %d imports [%s]' %
          (len(symbols), 'ok' if not unreviewed else 'BAD'))
    return versions_ok and not unreviewed


def load(path):
    with open(path, 'rb') as f:
        return f.read()


def address_profile(ra2):
    header = os.path.join(os.path.dirname(__file__), '..', 'include',
                          'RA2Addresses.h' if ra2 else 'YRAddresses.h')
    with open(header, encoding='utf-8') as source:
        return {name: int(value, 0) for name, value in re.findall(
            r'constexpr unsigned int (\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)', source.read())}


def native_image(game):
    d = load(game)
    nt, = struct.unpack_from('<I', d, 0x3C)
    count, = struct.unpack_from('<H', d, nt + 6)
    optsize, = struct.unpack_from('<H', d, nt + 20)
    stamp, = struct.unpack_from('<I', d, nt + 8)
    base, = struct.unpack_from('<I', d, nt + 52)
    sections = [struct.unpack_from('<IIII', d, nt + 24 + optsize + i * 40 + 8)
                for i in range(count)]
    def read(va, size):
        for vsize, rva, rawsize, rawptr in sections:
            if rva <= va - base and va - base + size <= rva + rawsize:
                off = rawptr + va - base - rva
                return d[off:off + size]
        return b''
    return base, stamp, read


def check_native_raster(game, profile):
    if not game:
        return True
    base, stamp, read = native_image(game)
    def at(field, size):
        return read(profile[field], size)
    prefix = bytes.fromhex('81 EC B4 00 00 00 53 56 8B F1 57')
    copy_prefix = bytes.fromhex('8B 44 24 1C 83 EC 20 53 56 8B F1')
    ok = base == 0x400000 and stamp == profile['kExeTimestamp'] and (
        at('DSurface_Fill', len(prefix)) == prefix and
        at('DSurface_FillSlot', 4) == struct.pack('<I', profile['DSurface_Fill']) and
        at('DSurface_TypeSlot', 4) == struct.pack('<I', profile['DSurface_Type']) and
        at('XSurface_Copy', len(copy_prefix)) == copy_prefix)
    cpu_ok = (at('BSurface_Vtable', 4) == struct.pack('<I', profile['BSurface_Delete']) and
              at('BSurface_Delete', 6) == bytes.fromhex('56 8B F1 8D 4E 14'))
    done = profile['Subtitle_DrawDone']
    subtitle_ok = (read(done - 5, 5) == b'\xE8' + struct.pack('<i', profile['BitText_DrawText'] - done) and
                   read(done + 0x16, 32) == bytes.fromhex(
                       '8B 44 24 10 8B 4A 0C 8B 54 24 0C 89 4B 0C 8B 4C 24 20 '
                       '89 55 00 89 7D 04 89 45 08 89 4D 0C 5D 5B'))
    print('native fill/copy/vtables: %s; CPU lifetime: %s; subtitle erase ABI: %s' %
          ('OK' if ok else 'DIFFER', 'OK' if cpu_ok else 'DIFFER', 'OK' if subtitle_ok else 'DIFFER'))
    return ok and cpu_ok and subtitle_ok


def check_runtime_hooks(exports, game, profile, ra2):
    read = native_image(game)[2] if game else None
    source = os.path.join(os.path.dirname(__file__), '..', 'src', 'RuntimeHookSites.inc')
    with open(source, encoding='utf-8') as file:
        entries = re.findall(r'VT_RUNTIME_HOOK\((\w+),\s*(\w+),\s*(\w+),\s*"([^"\n]*)",\s*"([^"\n]*)"\)', file.read())
    records = []
    ok = len(entries) == 16
    if read:
        ok = read(0x401000, 5) == bytes.fromhex('53 56 57 8B F1') and ok
    for field, handler, span, yr_bytes, ra_bytes in entries:
        address, length = profile[field], profile[span]
        expected = bytes.fromhex(''.join(re.findall(r'\\x([0-9A-Fa-f]{2})', ra_bytes if ra2 else yr_bytes)))
        valid = handler in exports and len(expected) == length and length >= 5
        if read:
            valid = read(address, length) == expected and valid
        records.append((address, length, handler))
        ok = valid and ok
        print('runtime %-36s 0x%08X/%d [%s]' % (handler, address, length, 'OK' if valid else 'BAD'))
    return ok, records


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
    import_symbols = []
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
                thunk_rva = fields[4] if fields[0] & 1 else fields[4] - image_base
            else:
                name_va = image_base + fields[3]
                thunk_rva = fields[0] or fields[4]
            module = read_cstr(name_va)
            imports.append(module)
            thunk = rva2off(thunk_rva)
            while True:
                symbol, = struct.unpack_from('<I', d, thunk)
                if not symbol:
                    break
                if symbol & 0x80000000:
                    name = '#%d' % (symbol & 0xFFFF)
                else:
                    symbol_va = (symbol if delayed and not fields[0] & 1
                                 else image_base + symbol)
                    name = read_cstr(symbol_va + 2)
                import_symbols.append((module or '<invalid>', name))
                thunk += 4
            off += descriptor_size
    independent = all(name and name.lower() not in ('phobos.dll', 'ares.dll') for name in imports)
    print('\nimports   : %s' % ', '.join(name or '<invalid>' for name in imports))
    print('no Phobos/Ares dependency: %s' % ('ok' if independent else 'BAD'))
    win7 = check_win7(struct.unpack_from('<HH', d, opt + 40),
                     struct.unpack_from('<HH', d, opt + 48), import_symbols)

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
    ok = independent and win7 and machine == 0x14C and magic == 0x10B and bool(characteristics & 0x2000)
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

    # Explicit executable selects the native audit profile; otherwise inspect
    # the adjacent game, or check both profiles' exported runtime handlers.
    game = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
    if not game:
        for name in ('gamemd.exe', 'game.exe'):
            candidate = os.path.join(os.path.dirname(path), name)
            if os.path.isfile(candidate):
                game = candidate
                break
    stamp = native_image(game)[1] if game else None
    if game and stamp not in (0x3BDF544E, 0x3B1EBBED):
        print('unsupported native executable timestamp: 0x%08X' % stamp)
        ok = False
    ra2 = stamp == 0x3B1EBBED
    profiles = [ra2] if game else [False, True]
    ok = records == [(0x401000, 5, 'VT_Bootstrap')] and ok
    for selected in profiles:
        profile = address_profile(selected)
        print('\nnative profile: %s' % ('RA2 1.006TUC' if selected else 'YR 1.001'))
        runtime_ok, runtime_records = check_runtime_hooks(exports, game, profile, selected)
        ok = runtime_ok and check_native_raster(game, profile) and ok
        native_records = [(profile['DSurface_Fill'], 6, 'DSurface::FillRectEx'),
                          (profile['XSurface_Copy'], 7, 'XSurface CPU copy'),
                          (profile['BSurface_Delete'], 6, 'BSurface deleting destructor')]
        if game:
            ok = check_phobos_overlap(game, records + runtime_records + native_records) and ok
    print('\nresult    : %s' % ('OK -- ready for Syringe' if ok else 'PROBLEMS FOUND'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
