#ifdef VT_BASELINE
#include "../build/render-qa/baseline-src/GlyphSource.h"
#include "../build/render-qa/baseline-src/PixelWriter.h"
#else
#include "../src/GlyphSource.h"
#include "../src/PixelWriter.h"
#endif
#include "../src/Takeover.h"
#include "../src/Logger.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <vector>

// Uses the production per-character takeover and a real game.fnt table.
// Run beside an INI copy, so QA never changes the game's config or log.
#pragma pack(push, 1)
struct FontData {
    unsigned int unknown, stride, lines, unknownC, unknown10, bytes;
    const unsigned short* symbols;
    unsigned char* bitmaps;
};
#pragma pack(pop)

static void WriteBmp(const char* path, const std::vector<unsigned short>& pixels, int w, int h) {
    int pitch = (w * 3 + 3) & ~3;
    std::vector<unsigned char> bmp(54 + pitch * h, 0);
    bmp[0] = 'B'; bmp[1] = 'M';
    *(unsigned int*)&bmp[2] = (unsigned int)bmp.size();
    *(unsigned int*)&bmp[10] = 54; *(unsigned int*)&bmp[14] = 40;
    *(int*)&bmp[18] = w; *(int*)&bmp[22] = h;
    *(unsigned short*)&bmp[26] = 1; *(unsigned short*)&bmp[28] = 24;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        unsigned short c = pixels[y * w + x];
        unsigned char* p = &bmp[54 + (h - y - 1) * pitch + x * 3];
        p[0] = (unsigned char)((c & 31) * 255 / 31);
        p[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
        p[2] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
    }
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(&bmp[0], 1, bmp.size(), f); fclose(f); }
}

int main(int argc, char** argv) {
    const char* output = argc > 1 ? argv[1] : "preview.bmp";
    bool check = argc > 2 && !strcmp(argv[2], "--check");
    bool line = argc > 3 && !strcmp(argv[3], "--line");
    bool scene = argc > 4 && !strcmp(argv[4], "--scene");
    FILE* f = fopen("F:\\Mental Omega\\game.fnt", "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
    std::vector<unsigned char> data(len);
    fread(&data[0], 1, len, f); fclose(f);
    std::vector<unsigned char> original = data;
    FontData fd = {};
    const unsigned int* hdr = (const unsigned int*)&data[0];
    fd.stride = hdr[2]; fd.lines = hdr[3]; fd.bytes = hdr[6];
    fd.symbols = (const unsigned short*)&data[28];
    fd.bitmaps = &data[28 + 131072];
    const int w = 720, h = 320;
    std::vector<unsigned short> pixels(w * h, 0);
    if (scene) {
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            // Deterministic dark terrain-like RGB565 background. The bottom
            // sample also exercises white mixed text on a flat UI grey.
            unsigned int noise = ((unsigned int)x * 1664525u + (unsigned int)y * 1013904223u) >> 25;
            int red = 3 + (noise & 3), green = 10 + (noise & 7), blue = 3 + (noise & 1);
            if (y >= 270) { red = blue = 3; green = 6; }
            pixels[y * w + x] = (unsigned short)((red << 11) | (green << 5) | blue);
        }
    }
    unsigned char bf[128] = {};
    *(void**)(bf + 4) = &fd; *(void**)(bf + 12) = &pixels[0];
    *(int*)(bf + 16) = w;
    *(int*)(bf + 56) = w - 1; *(int*)(bf + 60) = h - 1;
    const wchar_t* rows[] = {
        L"Phobos development build #48. Please test the build before shipping.",
        L"难度：普通",
        L"我们快到了。我们的部队已经抵达了尤里邪恶阴谋的核心。",
        L"我们很快就会从三个不同的方向对尤里的巨塔发起攻击。",
        L"我们必须守住它，将巨塔的人造晶体上的影响范围扩大。",
        L"Aa Bb Dd Gg Hh Ii Jj Ll Mm Ww  0123456789  .,;:!?()  。 ，：！？",
        L"任务目标一：为总攻行动肃清周边区域。",
        L"指挥官，命运和我们开了一个天大的玩笑，我们终于解开了尤里奇袭伦敦之谜——这让西格弗里德都大为震惊。",
        L"摧毁厄普西隆总部外围的基地， 为我们自己的基地扩张做准备。",
        L"他说：“保护‘心灵终结仪’，然后撤离。”",
        L"（中文）【任务目标】「保护基地」——完成……",
        L"中文，中文。中文：中文；中文！中文？中文、中文。",
        L"Mixed (中文) [MIDAS] 04:12, done. Test: 50%; OK!"
    };
    const int rowCount = sizeof(rows) / sizeof(rows[0]);
    int fails = 0;
    for (int r = 0; r < rowCount; ++r) {
        *(unsigned short*)(bf + 36) = r == 0 ? 0xF800 : (r >= 5 ? 0xFFFF : 0x07FF);
        int x = 4, y = 6 + r * 23;
        if (line && !vt::Takeover::BeginLine(bf, rows[r], -1, x, y, x, 0, 0, 0x43464D)) ++fails;
        for (const wchar_t* p = rows[r]; *p; ++p) {
            int oldWidth = fd.bitmaps[(fd.symbols[*p] - 1) * fd.bytes];
            int nextX = -1;
            bool drawn = line && vt::Takeover::TryLineBlit(bf, *p, x, y, -1, 0x43464D, &nextX);
            if (!drawn) drawn = vt::Takeover::TryBlit(bf, *p, x, y, -1, &nextX);
            if (!drawn) ++fails;
            if (nextX != x + oldWidth) ++fails;
            x = nextX;
        }
        vt::Takeover::EndLine(bf);
    }
    if (data != original) { printf("FAIL: game metrics/data changed\n"); ++fails; }
    WriteBmp(output, pixels, w, h);
    // Repeat draws at different origins must preserve original font metrics.
    for (int n = 0; n < 100; ++n) {
        int x = 3 + (n % 7), y = 29;
        for (const wchar_t* p = rows[1]; *p; ++p) {
            int expected = fd.bitmaps[(fd.symbols[*p] - 1) * fd.bytes], nx;
            vt::Takeover::TryBlit(bf, *p, x, y, -1, &nx);
            if (nx != x + expected) ++fails;
            x = nx;
        }
    }
    // Direct glyph checks supplement the surface/advance checks.
    vt::GlyphSource glyphs;
    glyphs.SetAntiAlias(true); glyphs.SetSupersample(vt::Cfg::Supersample());
    glyphs.SetHinting(vt::Cfg::ConfigInt("Hinting", 0));
    glyphs.SetClassAlign(vt::Cfg::ConfigInt("ClassAlign", 1) != 0);
    glyphs.SetFitMode(0);
    glyphs.Init(vt::Cfg::FontFile(), vt::Cfg::FontSizeLatin(), vt::Cfg::FontWeight(), 3, 16, vt::Cfg::BaselineRow());
    glyphs.SetSizes(vt::Cfg::FontSizeLatin(), vt::Cfg::FontSizeCJK());
    char latinFont[MAX_PATH] = {};
#ifndef VT_BASELINE
    vt::Cfg::ConfigStr("FontFileLatin", "", latinFont, sizeof(latinFont));
    if (latinFont[0] && !glyphs.SetLatinFont(latinFont)) ++fails;
    if (glyphs.FaceHandleFor(0x2014) != glyphs.FaceHandleFor(0x4E2D) ||
        glyphs.FaceHandleFor(0x201C) != glyphs.FaceHandleFor(0x4E2D) ||
        glyphs.FaceHandleFor('.') != glyphs.FaceHandle()) {
        printf("FAIL: mixed punctuation uses wrong face\n"); ++fails;
    }
#endif
    const vt::GlyphCell* narrow = glyphs.Get('M', 3, 0);
    const vt::GlyphCell* wide = glyphs.Get('M', 7, 0);
    if (narrow == wide || narrow->width != 3 || wide->width != 7) ++fails;
    const vt::GlyphCell* p0 = glyphs.Get('W', 11, 0);
    const vt::GlyphCell* p2 = glyphs.Get('W', 11, 2);
    if (p0 == p2) { printf("FAIL: subpixel phase cache collision\n"); ++fails; }
    // A rasterisation choice must not change natural line spacing. Compare
    // design advances across target-size normal/light and 4x light rendering.
    vt::GlyphSource metrics;
    metrics.SetAntiAlias(true); metrics.SetSupersample(1);
    metrics.Init(vt::Cfg::FontFile(), vt::Cfg::FontSizeLatin(), vt::Cfg::FontWeight(), 3, 16, vt::Cfg::BaselineRow());
    metrics.SetSizes(vt::Cfg::FontSizeLatin(), vt::Cfg::FontSizeCJK());
    if (latinFont[0]) metrics.SetLatinFont(latinFont);
    const wchar_t* metricSample = L"AVWim0g\x4E2D\x56FD\x3002";
    for (const wchar_t* p = metricSample; *p; ++p) {
        metrics.SetSupersample(1); metrics.SetHinting(0);
        const vt::GlyphCell* light = metrics.Get(*p, -1);
        if (!light) { ++fails; continue; }
        int expectedQ = light->advanceQ;
        metrics.SetHinting(1);
        const vt::GlyphCell* normal = metrics.Get(*p, -1);
        if (!normal || normal->advanceQ != expectedQ) { printf("FAIL: hinting changes U+%04X advance\n", *p); ++fails; }
        metrics.SetHinting(0); metrics.SetSupersample(4);
        const vt::GlyphCell* sampled = metrics.Get(*p, -1);
        if (!sampled || sampled->advanceQ != expectedQ) { printf("FAIL: sampling changes U+%04X advance\n", *p); ++fails; }
        metrics.SetSupersample(1);
        const vt::GlyphCell* phased = metrics.Get(*p, -1, 2);
        if (!phased || phased->advanceQ != expectedQ) { printf("FAIL: phase changes U+%04X advance\n", *p); ++fails; }
        const vt::GlyphCell* scaled = metrics.Get(*p, -1, 0, 950);
        if (!scaled || abs(scaled->advanceQ - (int)floor(expectedQ * 950.0 / 1024 + 0.5)) > 1) {
            printf("FAIL: scaled U+%04X natural advance is inconsistent\n", *p); ++fails;
        }
    }
    const vt::GlyphCell* box = glyphs.Get(0x56FD, 14, 0); // 国: continuous side strokes
    int top = -1, bottom = -1, blank = 0;
    for (int y = 0; y < 16; ++y) {
        int sum = 0; for (int x = 0; x < 24; ++x) sum += box->cov[y * 24 + x];
        if (sum) { if (top < 0) top = y; bottom = y; }
    }
    for (int y = top; y <= bottom; ++y) {
        int sum = 0; for (int x = 0; x < 24; ++x) sum += box->cov[y * 24 + x];
        if (!sum) ++blank;
    }
    printf("CJK box: occupied rows %d..%d, internal blank rows %d\n", top, bottom, blank);
    if (blank) ++fails;
    const wchar_t* sample = L"国难普部。，：；！？、（）【】「」“”‘’——…AaWMi.,:;!?()";
    for (const wchar_t* p = sample; *p; ++p) {
        int adv = fd.bitmaps[(fd.symbols[*p] - 1) * fd.bytes];
        const vt::GlyphCell* cell = glyphs.Get(*p, adv, 0);
        for (int y = 0; y < 16; ++y) for (int x = adv; x < 24; ++x)
            if (cell->cov[y * 24 + x]) {
                printf("FAIL: U+%04X coverage %d at x=%d exceeds advance %d\n", *p, cell->cov[y * 24 + x], x, adv);
                ++fails; y = 16; break;
            }
    }
#ifndef VT_BASELINE
    vt::SetDither(true);
    vt::SetLinearBlend(true);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        int last = 0;
        for (int cov = 0; cov <= 255; ++cov) {
            unsigned short c = vt::Blend(0, 0xF800, cov, vt::RGB565, x, y);
            int red = c >> 11;
            if ((c & 0x07FF) || red < last) ++fails;
            last = red;
            if (vt::Blend(0x1234, 0x1234, cov, vt::RGB565, x, y) != 0x1234) ++fails;
        }
    }
#endif
    printf("%s: %d failures; image %s\n", fails ? "FAIL" : "PASS", fails, output);
    return check && fails ? 1 : 0;
}
