// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Support.h"

// Safe string probes and observation logs; no render-state changes.
namespace vt::hooks
{
// ---------------------------------------------------------------- safety
bool RangeOk(const void *p, size_t bytes)
{
    if (!p)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    const BYTE *end = (const BYTE *)p + bytes;
    const BYTE *regionEnd = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
    return end <= regionEnd;
}

// Copy at most cap-1 wide characters.  No C++ objects in here: __try is not
// allowed in functions that require object unwinding.
bool SafeW(const void *p, wchar_t *dst, size_t cap)
{
    if (!cap)
        return false;
    dst[0] = 0;
    if (!RangeOk(p, sizeof(wchar_t)))
        return false;

    __try
    {
        const wchar_t *s = (const wchar_t *)p;
        size_t i = 0;
        for (; i + 1 < cap; ++i)
        {
            const wchar_t c = s[i];
            if (c == 0)
                break;
            dst[i] = c;
        }
        dst[i] = 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        dst[cap - 1] = 0;
        return false;
    }
}

bool Printable(wchar_t c)
{
    const unsigned int o = (unsigned int)c;
    if (o == 0x09 || o == 0x0A || o == 0x0D)
        return true; // line breaks in briefings
    if (o >= 0x20 && o < 0x7F)
        return true; // ASCII
    if (o >= 0x2010 && o <= 0x203F)
        return true; // punctuation
    if (o >= 0x3000 && o <= 0x303F)
        return true; // CJK punctuation
    if (o >= 0x4E00 && o <= 0x9FFF)
        return true; // CJK
    if (o >= 0xFF00 && o <= 0xFFEF)
        return true; // fullwidth forms
    return false;
}

// >= 1 character, >= 90% printable: single characters are legitimate text
// (digit rendering, width probes) and must not be dropped.
bool LooksLikeText(const wchar_t *s)
{
    if (!s || !*s)
        return false;
    int total = 0, good = 0;
    for (int i = 0; s[i] && i < 128; ++i)
    {
        ++total;
        if (Printable(s[i]))
            ++good;
    }
    return total >= 1 && good * 10 >= total * 9;
}

void PreviewPtr(const void *p, char *out, size_t cap)
{
    if (!p)
    {
        _snprintf_s(out, cap, _TRUNCATE, "null");
        return;
    }

    unsigned char b[16];
    bool ok = false;
    __try
    {
        memcpy(b, p, sizeof(b));
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }

    if (!ok)
    {
        _snprintf_s(out, cap, _TRUNCATE, "0x%08X unreadable", (unsigned int)(uintptr_t)p);
        return;
    }

    static const char *H = "0123456789ABCDEF";
    char hex[64];
    int k = 0;
    for (int i = 0; i < 16; ++i)
    {
        hex[k++] = H[b[i] >> 4];
        hex[k++] = H[b[i] & 0x0F];
        hex[k++] = ' ';
    }
    hex[k] = 0;

    char wide[16];
    int n = 0;
    for (int i = 0; i + 1 < 16 && n < 8; i += 2)
    {
        const unsigned int cu = (unsigned int)(b[i] | (b[i + 1] << 8));
        if (cu == 0)
            break;
        wide[n++] = (cu >= 0x20 && cu < 0x7F) ? (char)cu : '?';
    }
    wide[n] = 0;

    _snprintf_s(out, cap, _TRUNCATE, "0x%08X [%s] w=\"%s\"", (unsigned int)(uintptr_t)p, hex, wide);
}

std::string ArgDump(REGISTERS *R, int count, int firstOffset)
{
    std::string s;
    char tmp[48];
    for (int i = 0; i < count; ++i)
    {
        const int off = firstOffset + i * 4;
        _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "esp+0x%X=0x%08X ", off, R->Stack32(off));
        s += tmp;
    }
    return s;
}

std::string RegsDump(REGISTERS *R)
{
    char tmp[64];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "ecx=0x%08X edx=0x%08X ", R->ECX(), R->EDX());
    return std::string(tmp);
}

// Walk `count` stack dwords from esp+firstOffset, return the offset of the
// first one pointing at plausible text (0 if none).
int ScanForText(REGISTERS *R, int firstOffset, int count, wchar_t *buf, size_t cap)
{
    for (int i = 0; i < count; ++i)
    {
        const int off = firstOffset + i * 4;
        if (SafeW((const void *)(uintptr_t)R->Stack32(off), buf, cap) && LooksLikeText(buf))
            return off;
    }
    buf[0] = 0;
    return 0;
}

// --------------------------------------------------- reporting helpers
void ReportText(int hookId, REGISTERS *R, TextArg arg, int nArgs, int firstArgOffset, const char *what)
{
    wchar_t buf[512];
    const void *caller = (const void *)(uintptr_t)R->Stack32(0);

    bool ok = false;
    if (arg.ptr && SafeW(arg.ptr, buf, sizeof(buf) / sizeof(buf[0])) && LooksLikeText(buf))
    {
        ok = true;
    }
    else if (!arg.fromReg)
    {
        // documented offset failed -> note which offset the string really is at
        const int off = ScanForText(R, firstArgOffset, nArgs, buf, sizeof(buf) / sizeof(buf[0]));
        if (off)
        {
            ok = true;
            arg.offset = off;
        }
    }

    if (ok)
    {
        std::string note = RegsDump(R);
        if (arg.offset)
        {
            char pref[48];
            _snprintf_s(pref, sizeof(pref), _TRUNCATE, "text@esp+0x%X ", arg.offset);
            note += pref;
        }
        else if (arg.fromReg)
        {
            note += "text@edx ";
        }
        if (what && *what)
            note += what;
        note += " ";
        note += ArgDump(R, nArgs, firstArgOffset);
        vt::Log::Call(hookId, caller, buf, note.c_str());
    }
    else
    {
        char prev[200], prev2[200];
        PreviewPtr(arg.ptr, prev, sizeof(prev));
        PreviewPtr((const void *)(uintptr_t)R->Stack32(firstArgOffset), prev2, sizeof(prev2));

        std::string dump = RegsDump(R);
        dump += ArgDump(R, nArgs, firstArgOffset);
        char tail[480];
        _snprintf_s(tail, sizeof(tail), _TRUNCATE, " expect=%s arg1=%s", prev, prev2);
        dump += tail;
        vt::Log::Miss(hookId, caller, dump.c_str());
    }
}
} // namespace vt::hooks
