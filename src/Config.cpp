// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "ConfigState.h"
#include "ConfigParsing.h"
#include "Logger.h"
#include <cstdio>
#include <cstring>
#include <array>

namespace vt::config
{
bool g_enabled = true;
bool g_detailed = true;
bool g_blitDetail = false;
int g_maxUnique = parsing::maxUnique.defaultValue;
DWORD g_flushMs = parsing::flushMs.defaultValue;
char g_dir[MAX_PATH] = {0};
char g_logName[MAX_PATH] = "VectorText.log";
// Cached rendering values; loaded under the logger initialization lock.
int g_cfgMode = Cfg::Mode_Observe;
char g_cfgFont[MAX_PATH] = "C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf";
int g_cfgWeight = parsing::weight.defaultValue;
int g_cfgSizeLatin = parsing::latinSize.defaultValue;
int g_cfgSizeCJK = parsing::cjkSize.defaultValue;
int g_cfgBaseline = parsing::baseline.defaultValue;
bool g_cfgFit = true;
bool g_cfgAA = true;
bool g_cfgProbe = true;
int g_cfgDarkening = parsing::darkening.defaultValue;
double g_cfgGamma = parsing::gamma.defaultValue;
bool g_cfgVecMetrics = false;
int g_cfgSS = parsing::supersample.defaultValue;
bool g_cfgLinear = true;
bool g_cfgDither = true;
int g_cfgOutline = parsing::outline.defaultValue;
unsigned short g_cfgOutlineColor = parsing::outlineColor.defaultValue;
double g_cfgAdvScale = parsing::advanceScale.defaultValue;
bool g_cfgLegacy1252 = true;
bool g_cfgHiDPI = true;

void GetGameDir()
{
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash)
        *(slash + 1) = 0;
    strncpy_s(g_dir, path, _TRUNCATE);
}

namespace
{
using namespace parsing;
// A truncated INI token is invalid; never parse an apparently valid prefix.
struct ProfileValue
{
    std::array<char, 128> bytes{};
    std::string_view View() const noexcept { return bytes.data(); }
};
ProfileValue ReadValue(const char *ini, const char *key)
{
    ProfileValue value;
    const auto size = GetPrivateProfileStringA("VectorText", key, "", value.bytes.data(),
        static_cast<DWORD>(value.bytes.size()), ini);
    if (size >= value.bytes.size() - 1) value.bytes[0] = '\0';
    return value;
}
template <class Value, std::int64_t Default, std::int64_t Minimum, std::int64_t Maximum>
Value ReadNumber(const char *ini, const IntegerOption<Value, Default, Minimum, Maximum> &option)
{
    return option.Parse(ReadValue(ini, option.key).View());
}
double ReadNumber(const char *ini, const RealOption &option)
{
    return option.Parse(ReadValue(ini, option.key).View());
}
} // namespace

bool ReadBool(const char *ini, const char *key, bool def)
{
    return parsing::Boolean(ReadValue(ini, key).View()).value_or(def);
}

void ReadConfig()
{
    char ini[MAX_PATH];
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);

    g_enabled = ReadBool(ini, "Enabled", true);
    g_detailed = ReadBool(ini, "Detailed", true);
    g_blitDetail = ReadBool(ini, "LogBitFontBlitDetails", false);
    g_maxUnique = ReadNumber(ini, parsing::maxUnique);
    g_flushMs = ReadNumber(ini, parsing::flushMs);
    GetPrivateProfileStringA("VectorText", "LogFileName", "VectorText.log", g_logName, MAX_PATH, ini);

    // ---- cached rendering configuration ---------------------------
    char mode[32] = {0};
    GetPrivateProfileStringA("VectorText", "Mode", "observe", mode, sizeof(mode), ini);
    if (!_stricmp(mode, "off"))
        g_cfgMode = Cfg::Mode_Off;
    else if (!_stricmp(mode, "draw"))
        g_cfgMode = Cfg::Mode_Draw;
    else
        g_cfgMode = Cfg::Mode_Observe;

    GetPrivateProfileStringA("VectorText", "FontFile", "C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf", g_cfgFont, MAX_PATH,
                             ini);
    g_cfgWeight = ReadNumber(ini, parsing::weight);
    g_cfgSizeLatin = ReadNumber(ini, parsing::latinSize);
    g_cfgSizeCJK = ReadNumber(ini, parsing::cjkSize);
    g_cfgBaseline = ReadNumber(ini, parsing::baseline);
    g_cfgFit = ReadBool(ini, "FitToAdvance", true);
    g_cfgAA = ReadBool(ini, "AntiAlias", true);
    g_cfgProbe = ReadBool(ini, "Probe", true);
    g_cfgDarkening = ReadNumber(ini, parsing::darkening);
    g_cfgSS = ReadNumber(ini, parsing::supersample);
    g_cfgLinear = ReadBool(ini, "LinearBlend", true);
    g_cfgDither = ReadBool(ini, "Dither", true);
    g_cfgLegacy1252 = ReadBool(ini, "LegacyCodepage1252", true);
    g_cfgHiDPI = ReadBool(ini, "HiDPI", true);
    g_cfgOutline = ReadNumber(ini, parsing::outline);
    g_cfgOutlineColor = ReadNumber(ini, parsing::outlineColor);
    char metrics[32]{};
    GetPrivateProfileStringA("VectorText", "Metrics", "scaled", metrics, sizeof(metrics), ini);
    g_cfgVecMetrics = !_stricmp(metrics, "vector");
    g_cfgAdvScale = !_stricmp(metrics, "scaled") ? ReadNumber(ini, parsing::advanceScale) : 1.0;
    g_cfgGamma = ReadNumber(ini, parsing::gamma);
}

} // namespace vt::config

namespace vt
{
using namespace config;
// ---------------------------------------------------------------- Cfg ---
// Named rendering getters use Load() and the logger's cached INI values.
// Generic ConfigStr/ConfigBool/ConfigInt query the INI on each call; their
// callers cache settings where repeated reads would affect a hot path.
namespace Cfg
{
void Load()
{
    Log::Init();
}

int Mode()
{
    Load();
    return g_cfgMode;
}
const char *FontFile()
{
    Load();
    return g_cfgFont;
}
int FontWeight()
{
    Load();
    return g_cfgWeight;
}
int FontSizeLatin()
{
    Load();
    return g_cfgSizeLatin;
}
int FontSizeCJK()
{
    Load();
    return g_cfgSizeCJK;
}
int BaselineRow()
{
    Load();
    return g_cfgBaseline;
}
bool FitToAdvance()
{
    Load();
    return g_cfgFit;
}
bool AntiAlias()
{
    Load();
    return g_cfgAA;
}
bool Probe()
{
    Load();
    return g_cfgProbe;
}
int StemDarkening()
{
    Load();
    return g_cfgDarkening;
}
double Gamma()
{
    Load();
    return g_cfgGamma;
}
bool VectorMetrics()
{
    Load();
    return g_cfgVecMetrics;
}
double AdvanceScale()
{
    Load();
    return g_cfgAdvScale;
}
int Supersample()
{
    Load();
    return g_cfgSS;
}
bool LinearBlend()
{
    Load();
    return g_cfgLinear;
}
bool Dither()
{
    Load();
    return g_cfgDither;
}
int Outline()
{
    Load();
    return g_cfgOutline;
}
unsigned short OutlineColor()
{
    Load();
    return g_cfgOutlineColor;
}
bool LegacyCodepage1252()
{
    Load();
    return g_cfgLegacy1252;
}
bool HiDPI()
{
    Load();
    return g_cfgHiDPI;
}

void ConfigStr(const char *key, const char *def, char *out, int cch)
{
    char ini[MAX_PATH];
    if (!g_dir[0])
        GetGameDir();
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);
    GetPrivateProfileStringA("VectorText", key, def, out, (DWORD)cch, ini);
}

bool ConfigBool(const char *key, bool def)
{
    char ini[MAX_PATH];
    if (!g_dir[0])
        GetGameDir();
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);
    return ReadBool(ini, key, def);
}

int ConfigInt(const char *key, int def)
{
    char ini[MAX_PATH];
    if (!g_dir[0])
        GetGameDir();
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);
    const auto value = parsing::Integer(ReadValue(ini, key).View());
    if (!value || *value < parsing::intMin || *value > parsing::intMax) return def;
    return static_cast<int>(*value);
}
} // namespace Cfg
} // namespace vt
