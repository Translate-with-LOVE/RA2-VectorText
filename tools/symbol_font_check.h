// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

static bool WriteSymbolConfig()
{
    FILE* ini = fopen("VectorText.ini", "w");
    if (!ini) return false;
    fputs("[VectorText]\nMode=draw\nEnabled=true\nDetailed=false\nLineRender=true\n"
          "Metrics=game\nAdvanceScale=1\nAntiAlias=true\nSupersample=1\nSubpixel=true\n"
          "FontFile=C:\\Windows\\Fonts\\NotoSansSC-VF.ttf\n"
          "FontFileLatin=C:\\Windows\\Fonts\\arial.ttf\n"
          "FontFileSymbol=C:\\Windows\\Fonts\\seguisym.ttf\n"
          "FontSizeLatin=13\nFontSize=16\nFontSizeSymbol=19\nBaselineRow=13\n", ini);
    fclose(ini);
    return true;
}

static int CheckSymbolFont()
{
    const char* symbolPath = "C:\\Windows\\Fonts\\seguisym.ttf";
    vt::GlyphSource source;
    source.SetAntiAlias(true);
    CHECK(source.Init("C:\\Windows\\Fonts\\NotoSansSC-VF.ttf", 13, 450), "symbol fixture loads main font");
    source.SetSizes(13, 16);
    CHECK(source.SetLatinFont("C:\\Windows\\Fonts\\arial.ttf"), "symbol fixture loads Latin font");
    void* latin = source.FaceHandleFor('A');
    CHECK(!source.Get(0x26A1, -1) && !source.Get(0x26CF, -1), "unset symbol font preserves missing vector icons");
    CHECK(!source.SetSymbolFont("missing-symbol-font.ttf", 19), "missing file rejects without replacing faces");

    FT_Library lib = nullptr;
    FT_Face reference = nullptr;
    if (FT_Init_FreeType(&lib) || FT_New_Face(lib, symbolPath, 0, &reference)) return 2;
    for (int size : {11, 19})
    {
        void* cjk = source.FaceHandleFor(0x4E2D);
        CHECK(source.SetSymbolFont(symbolPath, size), "independent symbol font and size load");
        void* symbol = source.FaceHandleFor(0x26CF);
        CHECK(!source.SetSymbolFont("missing-symbol-font.ttf", size) && source.FaceHandleFor(0x26CF) == symbol,
              "invalid replacement preserves working symbol face");
        CHECK(source.FaceHandleFor('A') == latin && source.FaceHandleFor('5') == latin &&
              source.FaceHandleFor('+') == latin && source.FaceHandleFor('/') == latin &&
              source.FaceHandleFor(0x4E2D) == cjk && source.FaceHandleFor(0xFF0C) == cjk &&
              source.FaceHandleFor(0x2019) == cjk, "symbol font keeps ASCII, Chinese and punctuation faces");
        CHECK(source.FaceHandleFor(0x26CF) != latin, "pick selects configured symbol face");
        CHECK(source.KerningQuarter(0x26A1, '5') == 0, "different fonts have no cross-face kerning");
        for (int ss : {1, 2}) for (int density : {1, 2, 5, 8})
        {
            source.SetSupersample(ss);
            source.SetHighResolution(density > 1);
            source.SetHighResolutionScale(density);
            for (unsigned int cp : {0x26A1u, 0x26CFu, 0x231Au, 0x274Cu, 0x2192u})
            {
                const auto* cell = source.Get(cp, -1);
                CHECK(cell != nullptr, "configured symbols have vector glyphs");
                if (!cell) continue;
                const int rasterScale = density > 1 ? density : ss;
                FT_Set_Pixel_Sizes(reference, 0, size * rasterScale);
                FT_Set_Transform(reference, nullptr, nullptr);
                CHECK(!FT_Load_Char(reference, cp, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP | FT_LOAD_RENDER),
                      "independent symbol outline rasterizes");
                const auto& bitmap = reference->glyph->bitmap;
                if (density > 1)
                {
                    const auto* high = cell->raster2;
                    CHECK(high && high->scale == density && high->width == (int)bitmap.width &&
                          high->rows == (int)bitmap.rows && high->left == reference->glyph->bitmap_left &&
                          high->top == 13 * density - reference->glyph->bitmap_top,
                          "HiDPI uses independent symbol font, size and baseline");
                    if (high && high->width == (int)bitmap.width && high->rows == (int)bitmap.rows)
                        for (unsigned int r = 0; r < bitmap.rows; ++r)
                            CHECK(!memcmp(high->coverage.data() + r * bitmap.width,
                                          bitmap.buffer + r * bitmap.pitch, bitmap.width),
                                  "HiDPI symbol coverage equals independent FreeType raster");
                }
                else
                {
                    unsigned short actual[64 * 64]{}, expected[64 * 64]{};
                    unsigned int coverage[64 * 64]{};
                    vt::Target target{actual, 64, 0, 0, 63, 63};
                    vt::DrawCellAA(target, *cell, 12, 24, 16, 0x07FF, vt::RGB565);
                    for (unsigned int r = 0; r < bitmap.rows; ++r) for (unsigned int c = 0; c < bitmap.width; ++c)
                    {
                        const int x = (int)floor((12 * ss + reference->glyph->bitmap_left + (int)c) / (double)ss);
                        const int y = (int)floor((37 * ss - reference->glyph->bitmap_top + (int)r) / (double)ss);
                        if (x >= 0 && x < 64 && y >= 0 && y < 64)
                            coverage[y * 64 + x] += bitmap.buffer[r * bitmap.pitch + c];
                    }
                    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
                        expected[y * 64 + x] = vt::Blend(0, 0x07FF, (coverage[y * 64 + x] + ss * ss / 2) / (ss * ss),
                                                      vt::RGB565, x, y);
                    CHECK(!memcmp(actual, expected, sizeof(actual)), "logical symbol pixels equal independent outline");
                }
            }
        }
    }
    source.SetSupersample(1);
    source.SetSizes(17, 21);
    CHECK(((FT_Face)source.FaceHandleFor(0x26CF))->size->metrics.y_ppem == 19,
          "Latin and CJK size changes do not resize symbols");
    CHECK(source.SetSymbolFont("C:\\Windows\\Fonts\\arial.ttf", 19), "symbol font can be replaced");
    CHECK(source.FaceHandleFor(0x26CF) == source.FaceHandleFor('A') && !source.Get(0x26CF, -1),
          "missing symbol falls back instead of drawing .notdef");
    CHECK(source.SetSymbolFont("", 19), "empty path disables symbol face and invalidates caches");
    CHECK(!source.Get(0x26CF, -1), "disabled symbols retain native fallback");
    source.Shutdown();
    CHECK(source.Init("C:\\Windows\\Fonts\\arial.ttf", 13, 400) && !source.Get(0x26CF, -1),
          "reinitialization does not retain old symbol face");
    FT_Done_Face(reference);
    FT_Done_FreeType(lib);

    CHECK(vt::Cfg::FontSizeSymbol() == 19 && vt::Cfg::FontSizeLatin() == 13,
          "INI symbol size is independent of Latin size");
    vt::GlyphSource expected;
    expected.SetAntiAlias(true);
    CHECK(expected.Init("C:\\Windows\\Fonts\\NotoSansSC-VF.ttf", 13, 400), "row reference loads");
    expected.SetSizes(13, 16);
    CHECK(expected.SetLatinFont("C:\\Windows\\Fonts\\arial.ttf") && expected.SetSymbolFont(symbolPath, 19),
          "row reference configures three fonts");
    const wchar_t* text = L"\u26CF2/2 \u26A1+1710";
    Clear();
    CHECK(vt::Takeover::BeginLine(font, text, -1, 20, 12, 20, 200, 0, Caller),
          "production initialization reads FontFileSymbol and FontSizeSymbol");
    Draw(text, 20, 12);
    unsigned short referencePixels[W * H]{};
    vt::Target target{referencePixels, W, 0, 0, W - 1, H - 1};
    int penQ = 80;
    unsigned int previous = 0;
    for (const wchar_t* p = text; *p; ++p)
    {
        penQ += expected.KerningQuarter(previous, *p);
        const int x = (int)floor((penQ + 2) / 4.0), phase = penQ - x * 4;
        const auto* cell = expected.Get(*p, -1, phase);
        CHECK(cell != nullptr, "row reference has glyph");
        if (cell) { vt::DrawCellAA(target, *cell, x, 12, 16, 0x07FF, vt::RGB565); penQ += cell->advanceQ; }
        previous = *p;
    }
    CHECK(!memcmp(pixels, referencePixels, sizeof(pixels)), "counter icons and digits use configured fonts at independent sizes");
    printf("symbol font: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
