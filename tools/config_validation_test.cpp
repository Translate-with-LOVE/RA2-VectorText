// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "ConfigParsing.h"
#include "ConfigState.h"
#include <array>
#include <cstdio>
#include <string>
#include <atomic>
#include <thread>

// Link configuration alone: loading and querying it requires no logger stub.
namespace
{
int failures = 0;
void Check(bool ok, const char *label)
{
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}
using namespace vt::config::parsing;
static_assert(Boolean(" On ") == true && Boolean("FALSE") == false && !Boolean("2"));
void ParserEdges()
{
    Check(Integer("-9223372036854775808") == std::numeric_limits<std::int64_t>::min(), "signed minimum parsed without negation overflow");
    Check(Integer("9223372036854775807") == std::numeric_limits<std::int64_t>::max(), "signed maximum parsed exactly");
    Check(Integer(" -0x8000000000000000 ") == std::numeric_limits<std::int64_t>::min(), "negative hexadecimal minimum parsed exactly");
    for (auto invalid : {"", " ", "9223372036854775808", "-9223372036854775809", "18446744073709551616",
                         "+", "-", "0x", "0xQ", "--1", "+-1", "1.5", "1x", "1 0"})
        Check(!Integer(invalid), "malformed or overflowing signed integer rejected");
    Check(Integer(" +0Xff ") == 255 && Integer("010") == 10, "decimal and explicit hexadecimal grammar");
    Check(Real(" +1.25e0 ") == 1.25, "finite scientific notation parsed completely");
    for (auto invalid : {"nan", "NaN", "inf", "-inf", "1e999", "1e-999", "1.2px", "1 2", "+-1", "", "."})
        Check(!Real(invalid), "invalid, overflowing or non-finite real rejected");
}
struct Ini
{
    std::string path;
    Ini()
    {
        vt::config::GetGameDir();
        path = std::string(vt::config::g_dir) + "VectorText.ini";
        // This executable lives in its own CTest directory.
        FILE *file = nullptr;
        if (!fopen_s(&file, path.c_str(), "w") && file)
        { std::fputs("[VectorText]\n", file); std::fclose(file); }
        else Check(false, "isolated INI created");
        WritePrivateProfileStringA(nullptr, nullptr, nullptr, path.c_str());
    }
    void Set(const char *key, const char *value)
    {
        Check(WritePrivateProfileStringA("VectorText", key, value, path.c_str()) != FALSE, "INI test value written");
    }
};
void Initialization(Ini &ini)
{
    ini.Set("Gamma", "1.25");
    std::atomic<int> valid{0};
    std::array<std::thread, 8> readers;
    for (auto &reader : readers)
        reader = std::thread([&] {
            for (int i = 0; i < 100; ++i)
                if (vt::Cfg::Gamma() == 1.25) ++valid;
        });
    for (auto &reader : readers) reader.join();
    Check(valid == 800, "concurrent first readers see a fully initialized configuration");
    ini.Set("Gamma", "2.5");
    vt::Cfg::Load();
    Check(vt::Cfg::Gamma() == 1.25, "named getters retain the once-loaded configuration");
}
void BooleanOptions(Ini &ini)
{
    const auto expect = [&](const char *text, bool value) {
        ini.Set("Enabled", text); ini.Set("LogBitFontBlitDetails", text); ini.Set("GenericBool", text);
        vt::config::ReadConfig();
        Check(vt::config::g_enabled == value && vt::config::g_blitDetail == value,
              "cached true and false defaults both accept canonical Boolean tokens");
        Check(vt::Cfg::ConfigBool("GenericBool", true) == value && vt::Cfg::ConfigBool("GenericBool", false) == value,
              "generic getter shares the canonical Boolean grammar");
    };
    for (auto text : {"true", "TrUe", "yes", "ON", "1", "\tYES\t"}) expect(text, true);
    for (auto text : {"false", "FaLsE", "no", "OFF", "0", "\tNO\t"}) expect(text, false);
    for (auto text : {"2", "-1", "0x1", "+1", "01", "999999999999999999999999", "true junk", "", " ", "invalid"})
    {
        ini.Set("Enabled", text); ini.Set("LogBitFontBlitDetails", text); ini.Set("GenericBool", text);
        vt::config::ReadConfig();
        Check(vt::config::g_enabled && !vt::config::g_blitDetail, "invalid booleans preserve both cached defaults");
        Check(vt::Cfg::ConfigBool("GenericBool", true) && !vt::Cfg::ConfigBool("GenericBool", false),
              "invalid booleans preserve the caller's default");
    }
    ini.Set("Enabled", nullptr); ini.Set("LogBitFontBlitDetails", nullptr); ini.Set("GenericBool", nullptr);
    vt::config::ReadConfig();
    Check(vt::config::g_enabled && !vt::config::g_blitDetail && !vt::Cfg::ConfigBool("GenericBool", false),
          "missing booleans use their own defaults");
    const std::string longToken = "false" + std::string(130, ' ') + "junk";
    ini.Set("Enabled", longToken.c_str());
    vt::config::ReadConfig();
    Check(vt::config::g_enabled, "truncated Boolean cannot masquerade as a valid false prefix");
}
void NumberOptions(Ini &ini)
{
    struct FlushCase { const char *text; DWORD expected; };
    for (auto test : {FlushCase{nullptr, 2000}, {"", 2000}, {"invalid", 2000}, {"-1", 250}, {"0", 250},
                     {"249", 250}, {"250", 250}, {" +3000 ", 3000}, {"0xFA", 250},
                     {"2147483647", 2147483647}, {"2147483648", 2147483647},
                     {"4294967295", 2147483647}, {"-9223372036854775808", 250},
                     {"9223372036854775808", 2000}, {"12ms", 2000}})
    {
        ini.Set("FlushIntervalMs", test.text); vt::config::ReadConfig();
        Check(vt::config::g_flushMs == test.expected, "flush interval defaults and signed limits before DWORD conversion");
    }
    const std::string longNumber = "1000" + std::string(130, ' ') + "junk";
    ini.Set("FlushIntervalMs", longNumber.c_str()); vt::config::ReadConfig();
    Check(vt::config::g_flushMs == 2000, "truncated number cannot masquerade as a valid prefix");
    ini.Set("MaxUniqueStrings", "-1"); ini.Set("BaselineRow", "999"); ini.Set("Supersample", "-5");
    ini.Set("FontSizeLatin", "1"); ini.Set("FontSizeCJK", "5"); ini.Set("Outline", "999");
    ini.Set("OutlineColor", "0x1234"); ini.Set("Gamma", "nan"); ini.Set("AdvanceScale", "inf");
    ini.Set("Metrics", "scaled"); vt::config::ReadConfig();
    Check(vt::config::g_maxUnique == 16 && vt::Cfg::BaselineRow() == 32 && vt::Cfg::Supersample() == 1 &&
          vt::Cfg::FontSizeLatin() == 6 && vt::Cfg::FontSizeCJK() == 6 && vt::Cfg::Outline() == 2,
          "cached numeric ranges share their bounded signed policy");
    Check(vt::Cfg::OutlineColor() == 0x1234 && vt::Cfg::Gamma() == 1.0 && vt::Cfg::AdvanceScale() == 1.05,
          "color and non-finite real policies reach cached getters");
    for (auto test : {FlushCase{"-1", 0}, {"0x10000", 0xFFFF}, {"0xG", 0}, {"123junk", 0}})
    {
        ini.Set("OutlineColor", test.text); vt::config::ReadConfig();
        Check(vt::Cfg::OutlineColor() == test.expected, "RGB565 range is checked instead of wrapped with a bitmask");
    }
    ini.Set("Gamma", "0.1"); ini.Set("AdvanceScale", "3"); vt::config::ReadConfig();
    Check(vt::Cfg::Gamma() == 0.5 && vt::Cfg::AdvanceScale() == 2.0, "finite out-of-range real values are clamped");
    ini.Set("Gamma", "4"); ini.Set("AdvanceScale", "bad"); vt::config::ReadConfig();
    Check(vt::Cfg::Gamma() == 3.0 && vt::Cfg::AdvanceScale() == 1.05, "real upper bound and malformed default");
    ini.Set("Metrics", "game"); ini.Set("AdvanceScale", "2"); vt::config::ReadConfig();
    Check(vt::Cfg::AdvanceScale() == 1.0, "non-scaled metrics preserve unit advance");
    for (auto text : {"2147483648", "-2147483649", "999999999999999999999", "1junk", ""})
    {
        ini.Set("GenericInt", text);
        Check(vt::Cfg::ConfigInt("GenericInt", 7) == 7, "generic integer rejects unrepresentable or malformed values");
    }
    ini.Set("GenericInt", "-1"); Check(vt::Cfg::ConfigInt("GenericInt", 7) == -1, "generic signed integer retains its sign");
    ini.Set("GenericInt", "0x20"); Check(vt::Cfg::ConfigInt("GenericInt", 7) == 32, "generic integer supports explicit hex");
}
} // namespace
int main()
{
    ParserEdges(); Ini ini; Initialization(ini); BooleanOptions(ini); NumberOptions(ini);
    std::printf("config validation: %d failures\n", failures);
    return failures ? 1 : 0;
}
