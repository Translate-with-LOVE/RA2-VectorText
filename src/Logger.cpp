// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Logger.h"
#include "ConfigState.h"
#include "Takeover.h"
#include "GameAddresses.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <ctype.h>

namespace vt
{
using namespace config;
namespace
{
CRITICAL_SECTION g_cs;
bool g_csInit = false;
bool g_ready = false;
DWORD g_lastFlush = 0;
HANDLE g_file = INVALID_HANDLE_VALUE;

LONG64 g_calls[Hook_Count] = {0};
LONG64 g_lastReported[Hook_Count] = {0};

struct Entry
{
    unsigned long long count;
    unsigned int caller;
    std::string extra;
};

std::map<int, std::map<std::wstring, Entry>> g_unique;
size_t g_uniqueTotal = 0;

// per (hook, call site): how many unreadable / implausible calls we logged
std::map<int, std::map<unsigned int, int>> g_missCount;

const char *const kHookNames[Hook_Count] = {"Drawing::GetTextDimensions", "Drawing::PrintUnicode",
                                            "BitFont::GetTextDimension",  "BitText::Print",
                                            "BitText::DrawText",          "BitFont::Blit"};

// ---------------------------------------------------------------- utils
std::string Format(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return std::string(buf);
}

void WriteRaw(const char *data, DWORD len)
{
    if (g_file == INVALID_HANDLE_VALUE || len == 0)
        return;
    DWORD written = 0;
    WriteFile(g_file, data, len, &written, NULL);
}

void WriteLine(const std::string &s)
{
    WriteRaw(s.c_str(), (DWORD)s.size());
    WriteRaw("\r\n", 2);
}

std::string Narrow(const std::wstring &w)
{
    if (w.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0)
        return std::string();
    std::string out;
    out.resize((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, NULL, NULL);
    return out;
}

std::string EscapeUtf8(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (size_t i = 0; i < in.size(); ++i)
    {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back((char)c);
        }
        else if (c < 0x20)
        {
            char tmp[8];
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "\\x%02X", c);
            out += tmp;
        }
        else
        {
            out.push_back((char)c);
        }
    }
    return out;
}

LONG64 ReadCounter(int i)
{
    return InterlockedCompareExchange64(&g_calls[i], 0, 0);
}

// ------------------------------------------------------------- lifetime
void OpenLogFile()
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s%s", g_dir, g_logName);
    g_file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file != INVALID_HANDLE_VALUE)
    {
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF}; // so editors detect UTF-8
        WriteRaw((const char *)bom, 3);
    }
}

// ------------------------------------------------- executable identity
// Syringe's dwExeCRC is a plain CRC-32 (verified against syringe.log),
// so the game process can verify on its own which build it hooked.
unsigned int Crc32(const BYTE *data, size_t len, unsigned int crc)
{
    static unsigned int table[256];
    static bool tableReady = false;
    if (!tableReady)
    {
        for (unsigned int i = 0; i < 256; ++i)
        {
            unsigned int c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        tableReady = true;
    }

    crc = ~crc;
    for (size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFFu];
    return ~crc;
}

void WriteIdentity()
{
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    const unsigned int moduleBase = (unsigned int)(uintptr_t)GetModuleHandleA(NULL);

    DWORD fileSize = 0, timestamp = 0;
    unsigned int crc = 0;

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        fileSize = GetFileSize(h, NULL);

        BYTE header[0x400];
        DWORD got = 0;
        if (ReadFile(h, header, sizeof(header), &got, NULL) && got > 0x40)
        {
            DWORD lfanew = *(const DWORD *)(header + 0x3C);
            if (lfanew + 0x0C <= got)
                timestamp = *(const DWORD *)(header + lfanew + 8);
        }

        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        std::vector<BYTE> chunk(64 * 1024);
        DWORD read = 0;
        while (ReadFile(h, &chunk[0], (DWORD)chunk.size(), &read, NULL) && read > 0)
            crc = Crc32(&chunk[0], read, crc);
        CloseHandle(h);
    }

    WriteLine(Format("PROC  pid=%lu  moduleBase=0x%08X  %s", (unsigned long)GetCurrentProcessId(), moduleBase, path));
    WriteLine(Format("EXE   size=0x%08X timestamp=0x%08X crc32=0x%08X   (expected 0x%08X/0x%08X/0x%08X)", fileSize,
                     timestamp, crc, game::kExeSize, game::kExeTimestamp, game::kExeCRC));
    const bool steamYR = timestamp == yra::kExeTimestamp && fileSize == 0x0050A940 && crc == 0xA3F19485;
    if (!steamYR && (fileSize != game::kExeSize || crc != game::kExeCRC))
        WriteLine("WARN  unexpected executable -- executable differs from the reference profile; verify hook instructions before use.");
}

// -------------------------------------------------------------- summary
void WriteSummary(bool final)
{
    if (g_file == INVALID_HANDLE_VALUE)
        return;

    std::string stats;
    for (int i = 0; i < Hook_Count; ++i)
    {
        LONG64 now = ReadCounter(i);
        if (now == g_lastReported[i] && !final)
            continue;
        stats += Format("%s=%lld  ", kHookNames[i], (long long)now);
        g_lastReported[i] = now;
    }
    if (!stats.empty())
        WriteLine(Format("[%8lu ms] %-5s %s", GetTickCount(), final ? "FINAL" : "STATS", stats.c_str()));

    if (!final)
        return;

    typedef std::pair<int, std::pair<std::wstring, Entry>> Flat;
    std::vector<Flat> flat;
    for (std::map<int, std::map<std::wstring, Entry>>::iterator h = g_unique.begin(); h != g_unique.end(); ++h)
    {
        for (std::map<std::wstring, Entry>::iterator e = h->second.begin(); e != h->second.end(); ++e)
        {
            flat.push_back(Flat(h->first, *e));
        }
    }
    std::sort(flat.begin(), flat.end(),
              [](const Flat &a, const Flat &b) { return a.second.second.count > b.second.second.count; });

    WriteLine(Format("---- distinct strings: %u (cap %d) ----", (unsigned)g_uniqueTotal, g_maxUnique));
    for (size_t i = 0; i < flat.size(); ++i)
    {
        WriteLine(Format("UNIQ  %-34s x%-6llu first=0x%08X \"%s\"%s%s", kHookNames[flat[i].first],
                         flat[i].second.second.count, flat[i].second.second.caller,
                         EscapeUtf8(Narrow(flat[i].second.first)).c_str(),
                         flat[i].second.second.extra.empty() ? "" : "  ", flat[i].second.second.extra.c_str()));
    }
    WriteLine("---- end ----");
}

void FlushIfDue(bool force)
{
    DWORD now = GetTickCount();
    if (!force && (now - g_lastFlush) < g_flushMs)
        return;
    g_lastFlush = now;
    WriteSummary(force);
}
} // anonymous namespace

const char *HookName(int id)
{
    return (id >= 0 && id < Hook_Count) ? kHookNames[id] : "?";
}

namespace Log
{
bool Enabled()
{
    return g_ready && g_enabled;
}
bool Detailed()
{
    return g_ready && g_enabled && g_detailed;
}
bool WantBlitDetails()
{
    return g_ready && g_enabled && g_detailed && g_blitDetail;
}

void Note(const char *fmt, ...)
{
    if (!g_ready || g_file == INVALID_HANDLE_VALUE)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_cs);
    WriteLine(Format("[%8lu ms] %s", GetTickCount(), buf));
    LeaveCriticalSection(&g_cs);
}

void Prepare()
{
    if (g_csInit)
        return;
    InitializeCriticalSection(&g_cs);
    g_csInit = true;
}

void Init()
{
    if (g_ready)
        return;

    Prepare();

    EnterCriticalSection(&g_cs);
    if (!g_ready)
    {
        GetGameDir();
        ReadConfig();
        OpenLogFile();
        g_ready = true;
        g_lastFlush = GetTickCount();
        WriteLine("===================================================================");
        WriteLine(g_cfgMode == Cfg::Mode_Draw
                      ? " VectorText M1 -- vector text takeover ACTIVE (Mode=draw, own pixel writes)"
                  : g_cfgMode == Cfg::Mode_Off ? " VectorText -- disabled (Mode=off)"
                                               : " VectorText M1 -- observation only, no drawing behaviour is changed");
        WriteLine(Format(" log=%s%s  enabled=%d detailed=%d blitDetails=%d maxUnique=%d flush=%ums", g_dir, g_logName,
                         (int)g_enabled, (int)g_detailed, (int)g_blitDetail, g_maxUnique, (unsigned)g_flushMs));
        WriteLine(Format(" profile: %s; %s", game::Version, game::DllName));
        WriteLine(Format(" hooks: 0x%08X GetTextDimensions | 0x%08X PrintUnicode | 0x%08X GetTextDimension", game::Drawing_GetTextDimensions, game::Drawing_PrintUnicode, game::BitFont_GetTextDimension));
        WriteLine(Format("        0x%08X BitText::Print | 0x%08X DrawText | 0x%08X Blit", game::BitText_Print, game::BitText_DrawText, game::BitFont_Blit));
        WriteIdentity();
        WriteLine("===================================================================");
    }
    LeaveCriticalSection(&g_cs);
}

void Shutdown()
{
    if (!g_ready)
        return;
    EnterCriticalSection(&g_cs);
    WriteSummary(true);
    if (g_cfgMode == Cfg::Mode_Draw)
    {
        unsigned long long drawn = 0, skipped = 0, failed = 0, unknown = 0;
        Takeover::Stats(&drawn, &skipped, &failed, &unknown);
        WriteLine(Format(" M1 takeover: drawn=%llu skipped=%llu failed=%llu unknownGlyph=%llu", drawn, skipped, failed,
                         unknown));
        WriteLine(Format(" M1 refusals : %s", Takeover::ReasonSummary()));
        {
            char diag[256] = {0};
            Takeover::DiagLine(diag, sizeof(diag));
            WriteLine(Format(" M1 diagnostics: %s", diag));
        }
    }
    if (g_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_cs);
    g_ready = false;
}

void Count(int hookId)
{
    Init(); // lazy: the first hook call in *this* process opens the log
    if (!g_enabled)
        return;
    InterlockedIncrement64(&g_calls[hookId]);
    FlushIfDue(false);
}

void Call(int hookId, const void *caller, const wchar_t *text, const char *extra)
{
    Init(); // lazy: the first hook call in *this* process opens the log
    if (!g_enabled)
        return;

    if (!g_detailed)
    {
        Count(hookId);
        return;
    }
    std::wstring key(text ? text : L"");
    bool logNew = false;
    std::string line;

    EnterCriticalSection(&g_cs);
    g_calls[hookId]++;

    std::map<std::wstring, Entry> &perHook = g_unique[hookId];
    std::map<std::wstring, Entry>::iterator it = perHook.find(key);
    if (it == perHook.end())
    {
        if (g_uniqueTotal < (size_t)g_maxUnique)
        {
            Entry e;
            e.count = 1;
            e.caller = (unsigned int)(uintptr_t)caller;
            e.extra = extra ? extra : "";
            perHook[key] = e;
            ++g_uniqueTotal;
            logNew = g_detailed;
        }
    }
    else
    {
        it->second.count++;
    }

    if (logNew)
    {
        line = Format("[%8lu ms] NEW  %-34s caller=0x%08X text=\"%s\"%s%s", GetTickCount(), kHookNames[hookId],
                      (unsigned int)(uintptr_t)caller, EscapeUtf8(Narrow(key)).c_str(), (extra && *extra) ? "  " : "",
                      (extra && *extra) ? extra : "");
        WriteLine(line);
    }
    LeaveCriticalSection(&g_cs);

    FlushIfDue(false);
}

// Calls where the expected string argument was not readable/plausible.
// We keep the first few per call site: that is what resolves the real
// signature of the hook (single characters, empty strings, odd args).
void Miss(int hookId, const void *caller, const char *dump)
{
    Init();
    if (!g_enabled || !g_detailed)
        return;

    const unsigned int key = (unsigned int)(uintptr_t)caller;
    bool write = false;
    std::string line;

    EnterCriticalSection(&g_cs);
    g_calls[hookId]++;
    std::map<unsigned int, int> &perSite = g_missCount[hookId];
    int &n = perSite[key];
    if (n < 2)
    {
        ++n;
        write = true;
        line =
            Format("[%8lu ms] MISS %-34s caller=0x%08X %s", GetTickCount(), kHookNames[hookId], key, dump ? dump : "");
    }
    LeaveCriticalSection(&g_cs);

    if (write)
    {
        EnterCriticalSection(&g_cs);
        WriteLine(line);
        LeaveCriticalSection(&g_cs);
    }
    FlushIfDue(false);
}
} // namespace Log

} // namespace vt
