// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Logger.h"
#include "Takeover.h"
#include "../include/YRAddresses.h"

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
    namespace
    {
        CRITICAL_SECTION g_cs;
        bool  g_csInit   = false;
        bool  g_ready    = false;
        bool  g_enabled  = true;
        bool  g_detailed = true;
        bool  g_blitDetail = false;
        int   g_maxUnique = 4000;
        DWORD g_flushMs   = 2000;
        DWORD g_lastFlush = 0;
        HANDLE g_file = INVALID_HANDLE_VALUE;
        char  g_dir[MAX_PATH]     = { 0 };
        char  g_logName[MAX_PATH] = "VectorText.log";

        LONG64 g_calls[Hook_Count]        = { 0 };
        LONG64 g_lastReported[Hook_Count] = { 0 };

        // ---- M1 rendering configuration -----------------------------------
        int   g_cfgMode      = Cfg::Mode_Observe;
        char  g_cfgFont[MAX_PATH] = "C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf";
        int   g_cfgWeight    = 400;
        int   g_cfgSizeLatin = 13;
        int   g_cfgSizeCJK   = 16;
        int   g_cfgBaseline  = 13;
        bool  g_cfgFit       = true;
        bool  g_cfgAA        = true;
        bool  g_cfgProbe     = true;
        int   g_cfgDarkening = 0;
        double g_cfgGamma    = 1.0;
        bool  g_cfgVecMetrics = false;
        int   g_cfgSS        = 2;
        bool  g_cfgLinear    = true;
        bool  g_cfgDither    = true;
        int   g_cfgOutline   = 0;
        unsigned int g_cfgOutlineColor = 0x0000;
        double g_cfgAdvScale  = 1.0;
        bool g_cfgLegacy1252 = true;
        bool g_cfgHiDPI = true;

        struct Entry
        {
            unsigned long long count;
            unsigned int       caller;
            std::string        extra;
        };

        std::map<int, std::map<std::wstring, Entry> > g_unique;
        size_t g_uniqueTotal = 0;

        // per (hook, call site): how many unreadable / implausible calls we logged
        std::map<int, std::map<unsigned int, int> > g_missCount;

        const char* const kHookNames[Hook_Count] =
        {
            "Drawing::GetTextDimensions(0x4A59E0)",
            "Drawing::PrintUnicode(0x4A61C0)",
            "BitFont::GetTextDimension(0x433CF0)",
            "BitText::Print(0x434B90)",
            "BitText::DrawText(0x434CD0)",
            "BitFont::Blit(0x434120)"
        };

        // ---------------------------------------------------------------- utils
        std::string Format(const char* fmt, ...)
        {
            char buf[2048];
            va_list ap;
            va_start(ap, fmt);
            _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
            va_end(ap);
            return std::string(buf);
        }

        void WriteRaw(const char* data, DWORD len)
        {
            if (g_file == INVALID_HANDLE_VALUE || len == 0)
                return;
            DWORD written = 0;
            WriteFile(g_file, data, len, &written, NULL);
        }

        void WriteLine(const std::string& s)
        {
            WriteRaw(s.c_str(), (DWORD)s.size());
            WriteRaw("\r\n", 2);
        }

        std::string Narrow(const std::wstring& w)
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

        std::string EscapeUtf8(const std::string& in)
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
        void GetGameDir()
        {
            char path[MAX_PATH] = { 0 };
            GetModuleFileNameA(NULL, path, MAX_PATH);
            char* slash = strrchr(path, '\\');
            if (slash)
                *(slash + 1) = 0;
            strncpy_s(g_dir, path, _TRUNCATE);
        }

        bool ReadBool(const char* ini, const char* key, bool def)
        {
            char value[64] = {};
            GetPrivateProfileStringA("VectorText", key, "", value, sizeof(value), ini);
            char* first = value;
            while (isspace((unsigned char)*first)) ++first;
            char* end = first + strlen(first);
            while (end > first && isspace((unsigned char)end[-1])) --end;
            *end = 0;
            if (!_stricmp(first,"true") || !_stricmp(first,"yes") || !_stricmp(first,"on")) return true;
            if (!_stricmp(first,"false") || !_stricmp(first,"no") || !_stricmp(first,"off")) return false;
            char* numberEnd = NULL;
            const long number = strtol(first,&numberEnd,0);
            return numberEnd != first && !*numberEnd ? number != 0 : def;
        }

        void ReadConfig()
        {
            char ini[MAX_PATH];
            _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);

            g_enabled    = ReadBool(ini, "Enabled", true);
            g_detailed   = ReadBool(ini, "Detailed", true);
            g_blitDetail = ReadBool(ini, "LogBitFontBlitDetails", false);
            g_maxUnique  = GetPrivateProfileIntA("VectorText", "MaxUniqueStrings", 4000, ini);
            g_flushMs    = (DWORD)GetPrivateProfileIntA("VectorText", "FlushIntervalMs", 2000, ini);
            GetPrivateProfileStringA("VectorText", "LogFileName", "VectorText.log", g_logName, MAX_PATH, ini);

            if (g_maxUnique < 16)
                g_maxUnique = 16;
            if (g_flushMs < 250)
                g_flushMs = 250;

            // ---- M1 rendering configuration -------------------------------
            char mode[32] = { 0 };
            GetPrivateProfileStringA("VectorText", "Mode", "observe", mode, sizeof(mode), ini);
            if      (!_stricmp(mode, "off"))     g_cfgMode = Cfg::Mode_Off;
            else if (!_stricmp(mode, "draw"))    g_cfgMode = Cfg::Mode_Draw;
            else                                 g_cfgMode = Cfg::Mode_Observe;

            GetPrivateProfileStringA("VectorText", "FontFile",
                                     "C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf",
                                     g_cfgFont, MAX_PATH, ini);
            g_cfgWeight     = GetPrivateProfileIntA("VectorText", "FontWeight", 400, ini);
            g_cfgSizeLatin  = GetPrivateProfileIntA("VectorText", "FontSizeLatin", 13, ini);
            g_cfgSizeCJK    = GetPrivateProfileIntA("VectorText", "FontSizeCJK", 16, ini);
            g_cfgBaseline   = GetPrivateProfileIntA("VectorText", "BaselineRow", 13, ini);
            g_cfgFit        = ReadBool(ini, "FitToAdvance", true);
            g_cfgAA         = ReadBool(ini, "AntiAlias", true);
            g_cfgProbe      = ReadBool(ini, "Probe", true);
            g_cfgDarkening  = GetPrivateProfileIntA("VectorText", "StemDarkening", 0, ini);
            g_cfgSS         = GetPrivateProfileIntA("VectorText", "Supersample", 2, ini);
            g_cfgLinear     = ReadBool(ini, "LinearBlend", true);
            g_cfgDither     = ReadBool(ini, "Dither", true);
            g_cfgLegacy1252 = ReadBool(ini, "LegacyCodepage1252", true);
            g_cfgHiDPI      = ReadBool(ini, "HiDPI", true);
            g_cfgOutline    = GetPrivateProfileIntA("VectorText", "Outline", 0, ini);
            {
                char oc[32] = { 0 };
                GetPrivateProfileStringA("VectorText", "OutlineColor", "0x0000", oc, sizeof(oc), ini);
                g_cfgOutlineColor = (unsigned int)strtoul(oc, NULL, 0) & 0xFFFFu;
            }
            if (g_cfgSS < 1) g_cfgSS = 1;
            if (g_cfgSS > 4) g_cfgSS = 4;
            {
                char m[32] = { 0 };
                char s[32] = { 0 };
                GetPrivateProfileStringA("VectorText", "Metrics", "scaled", m, sizeof(m), ini);
                GetPrivateProfileStringA("VectorText", "AdvanceScale", "1.05", s, sizeof(s), ini);
                g_cfgVecMetrics = !_stricmp(m, "vector");
                g_cfgAdvScale = (!_stricmp(m, "scaled")) ? atof(s) : 1.0;
                if (g_cfgAdvScale < 1.0) g_cfgAdvScale = 1.0;
                if (g_cfgAdvScale > 2.0) g_cfgAdvScale = 2.0;
            }
            {
                char buf[32] = { 0 };
                GetPrivateProfileStringA("VectorText", "Gamma", "1.0", buf, sizeof(buf), ini);
                g_cfgGamma = atof(buf);
                if (g_cfgGamma < 0.5) g_cfgGamma = 0.5;
                if (g_cfgGamma > 3.0) g_cfgGamma = 3.0;
            }

            if (g_cfgSizeLatin < 6)  g_cfgSizeLatin = 6;
            if (g_cfgSizeCJK < 6)    g_cfgSizeCJK = 6;
            if (g_cfgBaseline < 1)   g_cfgBaseline = 1;
            if (g_cfgBaseline > 32)  g_cfgBaseline = 32;
        }

        void OpenLogFile()
        {
            char path[MAX_PATH];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s%s", g_dir, g_logName);
            g_file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (g_file != INVALID_HANDLE_VALUE)
            {
                const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };   // so editors detect UTF-8
                WriteRaw((const char*)bom, 3);
            }
        }

        // ------------------------------------------------- executable identity
        // Syringe's dwExeCRC is a plain CRC-32 (verified against syringe.log),
        // so the game process can verify on its own which build it hooked.
        unsigned int Crc32(const BYTE* data, size_t len, unsigned int crc)
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
            char path[MAX_PATH] = { 0 };
            GetModuleFileNameA(NULL, path, MAX_PATH);
            const unsigned int moduleBase = (unsigned int)(uintptr_t)GetModuleHandleA(NULL);

            DWORD fileSize = 0, timestamp = 0;
            unsigned int crc = 0;

            HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE)
            {
                fileSize = GetFileSize(h, NULL);

                BYTE header[0x400];
                DWORD got = 0;
                if (ReadFile(h, header, sizeof(header), &got, NULL) && got > 0x40)
                {
                    DWORD lfanew = *(const DWORD*)(header + 0x3C);
                    if (lfanew + 0x0C <= got)
                        timestamp = *(const DWORD*)(header + lfanew + 8);
                }

                SetFilePointer(h, 0, NULL, FILE_BEGIN);
                std::vector<BYTE> chunk(64 * 1024);
                DWORD read = 0;
                while (ReadFile(h, &chunk[0], (DWORD)chunk.size(), &read, NULL) && read > 0)
                    crc = Crc32(&chunk[0], read, crc);
                CloseHandle(h);
            }

            WriteLine(Format("PROC  pid=%lu  moduleBase=0x%08X  %s",
                             (unsigned long)GetCurrentProcessId(), moduleBase, path));
            WriteLine(Format("EXE   size=0x%08X timestamp=0x%08X crc32=0x%08X   (expected 0x%08X/0x%08X/0x%08X)",
                             fileSize, timestamp, crc,
                             yra::kExeSize, yra::kExeTimestamp, yra::kExeCRC));
            if (fileSize != yra::kExeSize || crc != yra::kExeCRC)
                WriteLine("WARN  unexpected executable -- addresses in YRAddresses.h may not match this build!");
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

            typedef std::pair<int, std::pair<std::wstring, Entry> > Flat;
            std::vector<Flat> flat;
            for (std::map<int, std::map<std::wstring, Entry> >::iterator h = g_unique.begin();
                 h != g_unique.end(); ++h)
            {
                for (std::map<std::wstring, Entry>::iterator e = h->second.begin();
                     e != h->second.end(); ++e)
                {
                    flat.push_back(Flat(h->first, *e));
                }
            }
            std::sort(flat.begin(), flat.end(),
                      [](const Flat& a, const Flat& b) { return a.second.second.count > b.second.second.count; });

            WriteLine(Format("---- distinct strings: %u (cap %d) ----", (unsigned)g_uniqueTotal, g_maxUnique));
            for (size_t i = 0; i < flat.size(); ++i)
            {
                WriteLine(Format("UNIQ  %-34s x%-6llu first=0x%08X \"%s\"%s%s",
                                 kHookNames[flat[i].first],
                                 flat[i].second.second.count,
                                 flat[i].second.second.caller,
                                 EscapeUtf8(Narrow(flat[i].second.first)).c_str(),
                                 flat[i].second.second.extra.empty() ? "" : "  ",
                                 flat[i].second.second.extra.c_str()));
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

    const char* HookName(int id)
    {
        return (id >= 0 && id < Hook_Count) ? kHookNames[id] : "?";
    }

    namespace Log
    {
        bool Enabled()         { return g_ready && g_enabled; }
        bool Detailed()        { return g_ready && g_enabled && g_detailed; }
        bool WantBlitDetails() { return g_ready && g_enabled && g_detailed && g_blitDetail; }

        void Note(const char* fmt, ...)
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
                    : g_cfgMode == Cfg::Mode_Off
                    ? " VectorText -- disabled (Mode=off)"
                    : " VectorText M1 -- observation only, no drawing behaviour is changed");
                WriteLine(Format(" log=%s%s  enabled=%d detailed=%d blitDetails=%d maxUnique=%d flush=%ums",
                                 g_dir, g_logName, (int)g_enabled, (int)g_detailed,
                                 (int)g_blitDetail, g_maxUnique, (unsigned)g_flushMs));
                WriteLine(" hooks: 0x4A59E0 GetTextDimensions | 0x4A61C0 PrintUnicode | 0x433CF0 GetTextDimension");
                WriteLine("        0x434B90 BitText::Print   | 0x434CD0 DrawText     | 0x434120 Blit (counters)");
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
                WriteLine(Format(" M1 takeover: drawn=%llu skipped=%llu failed=%llu unknownGlyph=%llu",
                                 drawn, skipped, failed, unknown));
                WriteLine(Format(" M1 refusals : %s", Takeover::ReasonSummary()));
                {
                    char diag[256] = { 0 };
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
            Init();                     // lazy: the first hook call in *this* process opens the log
            if (!g_enabled)
                return;
            InterlockedIncrement64(&g_calls[hookId]);
            FlushIfDue(false);
        }

        void Call(int hookId, const void* caller, const wchar_t* text, const char* extra)
        {
            Init();                     // lazy: the first hook call in *this* process opens the log
            if (!g_enabled)
                return;

            if (!g_detailed) { Count(hookId); return; }
            std::wstring key(text ? text : L"");
            bool logNew = false;
            std::string line;

            EnterCriticalSection(&g_cs);
            g_calls[hookId]++;

            std::map<std::wstring, Entry>& perHook = g_unique[hookId];
            std::map<std::wstring, Entry>::iterator it = perHook.find(key);
            if (it == perHook.end())
            {
                if (g_uniqueTotal < (size_t)g_maxUnique)
                {
                    Entry e;
                    e.count  = 1;
                    e.caller = (unsigned int)(uintptr_t)caller;
                    e.extra  = extra ? extra : "";
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
                line = Format("[%8lu ms] NEW  %-34s caller=0x%08X text=\"%s\"%s%s",
                              GetTickCount(), kHookNames[hookId],
                              (unsigned int)(uintptr_t)caller,
                              EscapeUtf8(Narrow(key)).c_str(),
                              (extra && *extra) ? "  " : "",
                              (extra && *extra) ? extra : "");
                WriteLine(line);
            }
            LeaveCriticalSection(&g_cs);

            FlushIfDue(false);
        }

        // Calls where the expected string argument was not readable/plausible.
        // We keep the first few per call site: that is what resolves the real
        // signature of the hook (single characters, empty strings, odd args).
        void Miss(int hookId, const void* caller, const char* dump)
        {
            Init();
            if (!g_enabled || !g_detailed)
                return;

            const unsigned int key = (unsigned int)(uintptr_t)caller;
            bool write = false;
            std::string line;

            EnterCriticalSection(&g_cs);
            g_calls[hookId]++;
            std::map<unsigned int, int>& perSite = g_missCount[hookId];
            int& n = perSite[key];
            if (n < 2)
            {
                ++n;
                write = true;
                line = Format("[%8lu ms] MISS %-34s caller=0x%08X %s",
                              GetTickCount(), kHookNames[hookId], key, dump ? dump : "");
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

    // ---------------------------------------------------------------- Cfg ---
    // Lazily reads the same VectorText.ini the logger uses.  Every accessor
    // goes through Init() so a call from a hook never touches the disk twice.
    namespace Cfg
    {
        void Load()
        {
            Log::Init();
        }

        int Mode()               { Load(); return g_cfgMode; }
        const char* FontFile()   { Load(); return g_cfgFont; }
        int FontWeight()         { Load(); return g_cfgWeight; }
        int FontSizeLatin()      { Load(); return g_cfgSizeLatin; }
        int FontSizeCJK()        { Load(); return g_cfgSizeCJK; }
        int BaselineRow()        { Load(); return g_cfgBaseline; }
        bool FitToAdvance()      { Load(); return g_cfgFit; }
        bool AntiAlias()         { Load(); return g_cfgAA; }
        bool Probe()             { Load(); return g_cfgProbe; }
        int  StemDarkening()     { Load(); return g_cfgDarkening; }
        double Gamma()           { Load(); return g_cfgGamma; }
        bool VectorMetrics()     { Load(); return g_cfgVecMetrics; }
        double AdvanceScale()    { Load(); return g_cfgAdvScale; }
        int  Supersample()       { Load(); return g_cfgSS; }
        bool LinearBlend()       { Load(); return g_cfgLinear; }
        bool Dither()            { Load(); return g_cfgDither; }
        int  Outline()           { Load(); return g_cfgOutline; }
        unsigned short OutlineColor() { Load(); return (unsigned short)g_cfgOutlineColor; }
        bool LegacyCodepage1252() { Load(); return g_cfgLegacy1252; }
        bool HiDPI()             { Load(); return g_cfgHiDPI; }

        void ConfigStr(const char* key, const char* def, char* out, int cch)
        {
            char ini[MAX_PATH];
            if (!g_dir[0])
                GetGameDir();
            _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);
            GetPrivateProfileStringA("VectorText", key, def, out, (DWORD)cch, ini);
        }

        bool ConfigBool(const char* key, bool def)
        {
            char ini[MAX_PATH];
            if (!g_dir[0]) GetGameDir();
            _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);
            return ReadBool(ini,key,def);
        }

        int ConfigInt(const char* key, int def)
        {
            char ini[MAX_PATH];
            if (!g_dir[0])
                GetGameDir();
            _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);
            return GetPrivateProfileIntA("VectorText", key, def, ini);
        }
    } // namespace Cfg
} // namespace vt
