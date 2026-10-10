// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include FT_MULTIPLE_MASTERS_H

static bool WriteWeightConfig()
{
    FILE* ini = fopen("VectorText.ini", "w");
    if (!ini) return false;
    fputs("[VectorText]\nEnabled=true\nDetailed=false\nMode=draw\nMetrics=game\nLineRender=true\n"
          "AdvanceScale=1\nAntiAlias=true\nSupersample=1\nHinting=0\n"
          "FontFile=C:\\Windows\\Fonts\\NotoSansSC-VF.ttf\n"
          "FontFileLatin=\nFontFileSymbol=C:\\Windows\\Fonts\\NotoSansSC-VF.ttf\n"
          "FontSize=16\nFontSizeLatin=13\nFontSizeSymbol=17\nBaselineRow=13\n"
          "FontWeight=600\nFontWeightLatin=300\nFontWeightSymbol=800\n", ini);
    fclose(ini);
    return true;
}

static FT_Fixed WeightCoordinate(FT_Face face)
{
    FT_MM_Var* axes = nullptr;
    if (FT_Get_MM_Var(face, &axes)) return -1;
    std::vector<FT_Fixed> coords(axes->num_axis);
    FT_Get_Var_Design_Coordinates(face, axes->num_axis, coords.data());
    FT_Fixed weight = -1;
    for (FT_UInt i = 0; i < axes->num_axis; ++i)
        if (axes->axis[i].tag == FT_MAKE_TAG('w','g','h','t')) weight = coords[i];
    FT_Done_MM_Var(face->glyph->library, axes);
    return weight;
}

static int CheckFontWeights()
{
    const char* path = "C:\\Windows\\Fonts\\NotoSansSC-VF.ttf";
    vt::GlyphSource source;
    source.SetAntiAlias(true);
    CHECK(source.Init(path, 13, 600), "variable main font loads");
    source.SetSizes(13, 16);
    CHECK(source.SetLatinFont("", 300), "same font file supports independent Latin weight");
    CHECK(source.SetSymbolFont(path, 17, 800), "variable symbol font loads independently");
    CHECK(vt::Cfg::FontWeight() == 600 && vt::Cfg::FontWeightLatin() == 300 &&
          vt::Cfg::FontWeightSymbol() == 800 && vt::Cfg::FontSize() == 16,
          "runtime configuration exposes three weights and main size");
    FT_Library lib = nullptr;
    FT_Face reference = nullptr;
    if (FT_Init_FreeType(&lib) || FT_New_Face(lib, path, 0, &reference)) return 2;
    const unsigned int cps[] = {'A', 0x4E2D, 0xA9};
    const int weights[] = {300, 600, 800}, sizes[] = {13, 16, 17};
    for (int ss : {1, 2}) for (int density : {1, 2, 5, 8})
    {
        source.SetSupersample(ss);
        source.SetHighResolution(density > 1);
        source.SetHighResolutionScale(density);
        for (int i = 0; i < 3; ++i)
        {
            CHECK(WeightCoordinate((FT_Face)source.FaceHandleFor(cps[i])) == weights[i] * 65536,
                  "main, Latin and symbol faces retain independent weight after supersampling");
            const auto* cell = source.Get(cps[i], -1);
            CHECK(cell != nullptr, "weighted glyph loads");
            if (!cell) continue;
            const int rasterScale = density > 1 ? density : ss;
            FT_Fixed coordinate = weights[i] * 65536;
            FT_Set_Var_Design_Coordinates(reference, 1, &coordinate);
            FT_Set_Pixel_Sizes(reference, 0, sizes[i] * rasterScale);
            FT_Set_Transform(reference, nullptr, nullptr);
            CHECK(!FT_Load_Char(reference, cps[i], FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP | FT_LOAD_RENDER),
                  "independent weighted reference loads");
            const auto& bitmap = reference->glyph->bitmap;
            if (density > 1)
            {
                const auto* high = cell->raster2;
                CHECK(high && high->width == (int)bitmap.width && high->rows == (int)bitmap.rows &&
                      high->left == reference->glyph->bitmap_left && high->top == 13 * density - reference->glyph->bitmap_top,
                      "high-resolution face uses correct font weight and size");
                if (high && high->width == (int)bitmap.width && high->rows == (int)bitmap.rows)
                    for (unsigned int r = 0; r < bitmap.rows; ++r)
                        CHECK(!memcmp(high->coverage.data() + r * bitmap.width,
                                      bitmap.buffer + r * bitmap.pitch, bitmap.width),
                              "weighted HiDPI pixels equal independent variable-font raster");
            }
            else
            {
                unsigned short actual[64 * 64]{}, expected[64 * 64]{};
                unsigned int coverage[64 * 64]{};
                vt::Target target{actual,64,0,0,63,63};
                vt::DrawCellAA(target, *cell, 12, 24, 16, 0x07FF, vt::RGB565);
                for (unsigned int r = 0; r < bitmap.rows; ++r) for (unsigned int c = 0; c < bitmap.width; ++c)
                {
                    const int x = (int)floor((12 * ss + reference->glyph->bitmap_left + (int)c) / (double)ss);
                    const int y = (int)floor((37 * ss - reference->glyph->bitmap_top + (int)r) / (double)ss);
                    if (x >= 0 && x < 64 && y >= 0 && y < 64)
                        coverage[y * 64 + x] += bitmap.buffer[r * bitmap.pitch + c];
                }
                for (int y=0; y<64; ++y) for (int x=0; x<64; ++x)
                    expected[y*64+x] = vt::Blend(0, 0x07FF, (coverage[y*64+x]+ss*ss/2)/(ss*ss), vt::RGB565, x, y);
                CHECK(!memcmp(actual,expected,sizeof(actual)), "logical pixels use independent weights");
            }
        }
    }
    // Equal sizes and the same file still require distinct variable faces.
    source.SetSizes(13, 13);
    source.SetSupersample(1);
    CHECK(WeightCoordinate((FT_Face)source.FaceHandleFor('A')) == 300 * 65536 &&
          WeightCoordinate((FT_Face)source.FaceHandleFor(0x4E2D)) == 600 * 65536,
          "equal sizes do not merge different weights");
    CHECK(source.SetLatinFont("", 900) && source.SetSymbolFont(path, 17, 200), "font changes update independent weights");
    CHECK(WeightCoordinate((FT_Face)source.FaceHandleFor('A')) == 900 * 65536 &&
          WeightCoordinate((FT_Face)source.FaceHandleFor(0xA9)) == 200 * 65536 &&
          WeightCoordinate((FT_Face)source.FaceHandleFor(0x4E2D)) == 600 * 65536,
          "changing Latin and symbol weights does not alter main weight");
    CHECK(source.SetSymbolFont(path, 17, INT_MAX) &&
          WeightCoordinate((FT_Face)source.FaceHandleFor(0xA9)) == 900 * 65536,
          "out-of-range weight clamps to font axis without overflowing fixed point");

    // Production initialization must also honor weights when Latin shares FontFile.
    vt::GlyphSource expected;
    expected.SetAntiAlias(true);
    CHECK(expected.Init(path, 13, 600), "production weight reference loads");
    expected.SetSizes(13, 16);
    CHECK(expected.SetLatinFont("", 300) && expected.SetSymbolFont(path, 17, 800), "production reference has three weights");
    for (int i=0; i<3; ++i)
    {
        wchar_t text[] = {(wchar_t)cps[i], 0};
        Clear();
        CHECK(vt::Takeover::BeginLine(font,text,1,20,12,20,200,0,Caller), "production glyph line prepares");
        Draw(text,20,12);
        unsigned short referencePixels[W*H]{};
        vt::Target target{referencePixels,W,0,0,W-1,H-1};
        const auto* cell = expected.Get(cps[i],-1);
        CHECK(cell != nullptr,"production reference has glyph");
        if(cell) vt::DrawCellAA(target,*cell,20,12,16,0x07FF,vt::RGB565);
        CHECK(!memcmp(pixels,referencePixels,sizeof(pixels)), "production renderer honors configured main, Latin and symbol weights");
    }
    FT_Done_Face(reference);
    FT_Done_FreeType(lib);
    printf("independent font weights: %s (%d failures)\n",failures ? "FAIL" : "PASS",failures);
    return failures ? 1 : 0;
}
