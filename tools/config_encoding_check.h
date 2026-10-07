// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Each mode runs in a fresh process because named rendering settings are cached.
static int CheckConfigEncoding(const char* mode)
{
    const bool numeric = !strcmp(mode,"numeric");
    const bool disabled = !strcmp(mode,"false");
    const bool missing = !strcmp(mode,"missing");
    const bool invalid = !strcmp(mode,"invalid");
    const char* value = numeric ? "1" : disabled ? "FaLsE" : invalid ? "invalid" : "TrUe";
    FILE* ini = fopen("VectorText.ini","w");
    if (!ini) return 2;
    fprintf(ini,"[VectorText]\nEnabled=%s\nDetailed=%s\nLogBitFontBlitDetails=%s\n"
        "Mode=draw\nFitToAdvance=%s\nAntiAlias=%s\nProbe=%s\n"
        "LinearBlend=%s\nDither=%s\nLineRender=%s\nDynamicTextWidth=%s\nPresent32=%s\nSubpixel=%s\n"
        "FontFile=C:\\Windows\\Fonts\\NotoSansSC-VF.ttf\nFontFileLatin=C:\\Windows\\Fonts\\arial.ttf\n"
        "FontSizeLatin=13\nFontSizeCJK=16\nSupersample=1\n"
        "BoolYes=YeS\nBoolOff=oFf\nBoolOne=1\nBoolZero=0\nBoolInvalid=not-a-bool\n",
        value,value,value,value,value,value,value,value,value,value,value,value);
    if (!missing) fprintf(ini,"LegacyCodepage1252=%s\nHiDPI=%s\n",value,value);
    fclose(ini);
    const bool enabled = !disabled;
    CHECK(vt::Cfg::FitToAdvance()==enabled && vt::Cfg::AntiAlias()==enabled &&
          vt::Cfg::Probe()==enabled &&
          vt::Cfg::LinearBlend()==enabled && vt::Cfg::Dither()==enabled,
          "cached rendering booleans parse mixed-case true/false, numeric and invalid defaults");
    CHECK(vt::Log::Enabled()==enabled && vt::Log::Detailed()==enabled &&
          vt::Log::WantBlitDetails()==(enabled && !invalid),"logging flags use the same boolean parser");
    CHECK(vt::Cfg::ConfigBool("Present32",true)==enabled &&
          vt::Cfg::ConfigBool("Subpixel",true)==enabled &&
          vt::Cfg::ConfigBool("DynamicTextWidth",true)==enabled &&
          vt::Cfg::ConfigBool("LineRender",true)==enabled,"runtime flags use the same boolean parser");
    CHECK(vt::Cfg::ConfigBool("BoolYes",false) && !vt::Cfg::ConfigBool("BoolOff",true) &&
          vt::Cfg::ConfigBool("BoolOne",false) && !vt::Cfg::ConfigBool("BoolZero",true) &&
          vt::Cfg::ConfigBool("BoolInvalid",true) && !vt::Cfg::ConfigBool("BoolInvalid",false) &&
          vt::Cfg::ConfigBool("Missing",true) && !vt::Cfg::ConfigBool("Missing",false),
          "yes/no, 1/0 and missing/invalid values preserve the supplied default");
    CHECK(vt::Cfg::HiDPI()==enabled,
          "HiDPI uses the new boolean; absent/invalid values preserve its enabled default");
    CHECK(vt::Cfg::LegacyCodepage1252()==enabled,"legacy codepage switch defaults on and supports strict Unicode mode");
    CHECK(vt::Cfg::FontSizeCJK()==16 && vt::Cfg::Supersample()==1,
          "numeric font and sampling options retain integer semantics");

    vt::GlyphSource source;
    source.SetAntiAlias(true);
    CHECK(source.Init("C:\\Windows\\Fonts\\NotoSansSC-VF.ttf",13,450),"encoding fixture loads CJK face");
    source.SetSizes(13,16);
    CHECK(source.SetLatinFont("C:\\Windows\\Fonts\\arial.ttf"),"encoding fixture loads Latin face");
    source.SetLegacyCodepage1252(vt::Cfg::LegacyCodepage1252());
    source.SetHighResolution(vt::Cfg::HiDPI());
    int mapped = 0, undefined = 0;
    for (unsigned int cp=0x80;cp<=0x9F;++cp) {
        // Windows' explicit CP1252 decoder is independent of our lookup table.
        if (cp==0x81 || cp==0x8D || cp==0x8F || cp==0x90 || cp==0x9D) {
            CHECK(source.RenderCodepoint(cp)==cp,"undefined CP1252 slots remain unchanged"); ++undefined; continue;
        }
        const char byte=(char)cp;
        wchar_t unicode=0;
        CHECK(MultiByteToWideChar(1252,0,&byte,1,&unicode,1)==1,"Windows CP1252 reference decoding succeeds");
        CHECK(source.RenderCodepoint(cp)==(enabled ? (unsigned int)unicode : cp),"all defined legacy slots follow configured interpretation");
        if (enabled) {
            CHECK(source.Get(cp,-1)==source.Get(unicode,-1),"legacy slot and real Unicode share glyph, metrics and cache");
            CHECK(source.KerningQuarter('A',cp)==source.KerningQuarter('A',unicode),"legacy and Unicode kerning agree");
            CHECK(source.FaceHandleFor(cp)==source.FaceHandleFor(unicode),"legacy punctuation selects the correct font face");
        }
        ++mapped;
    }
    CHECK(mapped==27 && undefined==5,"only the 27 defined slots in the 32-position range are remapped");
    CHECK(source.RenderCodepoint(',')==',' && source.RenderCodepoint(0xFF0C)==0xFF0C &&
          source.RenderCodepoint(0x2019)==0x2019 && source.RenderCodepoint(0x4E2D)==0x4E2D,
          "ASCII, fullwidth punctuation, correct Unicode and Chinese stay unchanged");
    const vt::GlyphCell* high=source.Get('M',-1);
    CHECK(high && (high->raster2!=NULL)==vt::Cfg::HiDPI(),"HiDPI switch controls output-resolution glyph allocation");
    source.SetLegacyCodepage1252(true); source.Get(0x92,-1);
    source.SetLegacyCodepage1252(false);
    CHECK(source.RenderCodepoint(0x92)==0x92,"switching to Unicode clears legacy interpretation");
    source.SetLegacyCodepage1252(true);
    CHECK(source.Get(0x92,-1)==source.Get(0x2019,-1),"cache invalidation restores correct glyph after toggling");
    printf("config/encoding %s: %s (%d failures)\n",mode,failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
