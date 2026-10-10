// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
// ===========================================================================
//  takeover_test.cpp -- offline verification of the per-glyph fallback.
//
//  Builds a *synthetic* BitFont object that mirrors the layout the engine uses
//  (verified against BitFont::Blit / BitFont::Lock) and feeds the real game.fnt
//  symbol table + glyph bitmaps into it.  Then it calls vt::Takeover::TryBlit
//  exactly like the hook does and checks:
//
//    1. Metrics=game returns native advance + tracking, including clipped glyphs
//    2. pixels really land in the surface, on the right rows
//    3. the colour comes from BitFont+0x24 when the colour argument is -1,
//       and from the argument otherwise
//    4. AA mode blends (edge pixels differ from the pure colour)
//    5. out-of-bounds drawing is clipped to BitFont's bounds
//    6. an unlocked surface (BitFont+0x0C == 0) is refused -> engine falls back
//    7. a character with no glyph in game.fnt is refused -> engine handles it
//    8. positive/negative tracking at BitFont+0x2C matches the native return
//
//  usage: takeover_test.exe [--aa 0|1] [--out file.bmp] [--fnt path]
// ===========================================================================

#include "../src/Takeover.h"
#include "../src/Logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wchar.h>
#include <initializer_list>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); } \
                              else { printf("  [ ok ] "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ------------------------------------------------------------ game.fnt -----
static unsigned char* g_fnt = NULL;
static int g_lines = 16, g_symbolSize = 49, g_glyphOff = 0;
static unsigned short* g_map = NULL;
static const unsigned char* g_bitmaps = NULL;
static int g_glyphCount = 0;

static bool LoadGameFnt(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
    g_fnt = (unsigned char*)malloc(size);
    if (!g_fnt || fread(g_fnt, 1, size, f) != (size_t)size) { fclose(f); return false; }
    fclose(f);
    if (*(unsigned int*)g_fnt != 0x546E6F66u) return false;
    const unsigned int* hdr = (const unsigned int*)(g_fnt + 4);
    g_lines = (int)hdr[2];
    g_symbolSize = (int)hdr[5];
    g_glyphCount = (int)hdr[4];
    g_map = (unsigned short*)(g_fnt + 0x1C);
    g_glyphOff = 0x1C + 0x20000;
    g_bitmaps = g_fnt + g_glyphOff;
    return true;
}

static int AdvOf(unsigned int ch)          // original advance, straight from game.fnt
{
    if (ch >= 0x10000 || !g_map[ch]) return -1;
    return g_bitmaps[(size_t)(g_map[ch] - 1) * g_symbolSize];
}

// ------------------------------------------------------------- surface -----
static const int W = 320, H = 64;
static unsigned short g_surf[W * H];
static const unsigned short BG = 0x0000;
static const unsigned short FG = 0xC618;

// ------------------------------------------------- synthetic engine objects
#pragma pack(push, 1)
struct FakeInternal                   // mirrors the game.fnt parse result
{
    unsigned int  unknown00;
    unsigned int  stride;             // +0x04
    int           lines;              // +0x08
    unsigned int  unknown0C;
    unsigned int  unknown10;
    unsigned int  symbolBytes;        // +0x14
    const unsigned short* symTable;   // +0x18
    const unsigned char*  bitmaps;    // +0x1C
};
#pragma pack(pop)

static unsigned char g_bitFont[0x80];

static void InitBitFont(FakeInternal* in, unsigned short* surf, int pitch, unsigned short color,
                        int L, int T, int Rr, int B)
{
    memset(g_bitFont, 0, sizeof(g_bitFont));
    *(void**)(g_bitFont + 0x04) = in;
    *(void**)(g_bitFont + 0x0C) = surf;
    *(int*)(g_bitFont + 0x10) = pitch;
    *(unsigned short*)(g_bitFont + 0x24) = color;
    *(int*)(g_bitFont + 0x30) = L;
    *(int*)(g_bitFont + 0x34) = T;
    *(int*)(g_bitFont + 0x38) = Rr;
    *(int*)(g_bitFont + 0x3C) = B;
}

static int CountInk()
{
    int n = 0;
    for (int i = 0; i < W * H; ++i) if (g_surf[i] != BG) ++n;
    return n;
}

static void DumpArt(int y0, int y1)
{
    for (int y = y0; y <= y1; ++y)
    {
        printf("   ");
        for (int x = 0; x < 160; ++x)
            putchar(g_surf[y * W + x] != BG ? '#' : '.');
        printf("\n");
    }
}

static void WriteBmp(const char* path)
{
    const int rowBytes = W * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const int dataSize = (rowBytes + pad) * H;
    const int fileSize = 54 + dataSize;
    unsigned char* img = (unsigned char*)malloc(fileSize);
    if (!img) return;
    memset(img, 0, 54);
    img[0] = 'B'; img[1] = 'M';
    *(int*)(img + 2) = fileSize; *(int*)(img + 10) = 54; *(int*)(img + 14) = 40;
    *(int*)(img + 18) = W; *(int*)(img + 22) = H;
    *(short*)(img + 26) = 1; *(short*)(img + 28) = 24;
    *(int*)(img + 34) = dataSize;
    for (int y = 0; y < H; ++y)
    {
        unsigned char* dst = img + 54 + (size_t)(H - 1 - y) * (rowBytes + pad);
        for (int x = 0; x < W; ++x)
        {
            const unsigned short c = g_surf[y * W + x];
            dst[x * 3 + 0] = (unsigned char)((c & 0x1F) * 255 / 31);
            dst[x * 3 + 1] = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
            dst[x * 3 + 2] = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
        }
    }
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(img, 1, fileSize, f); fclose(f); }
    free(img);
}

static void WriteIni(int aa)
{
    FILE* f = fopen("VectorText.ini", "w");
    if (!f) return;
    fprintf(f, "[VectorText]\nEnabled=1\nMode=draw\nAntiAlias=%d\n"
               "FontFile=C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf\n"
               "FontWeight=400\nFontSizeLatin=13\nFontSize=16\nBaselineRow=13\nFitToAdvance=1\n",
            aa ? 1 : 0);
    fclose(f);
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    const char* fnt = "..\\..\\..\\game.fnt";
    const char* out = "takeover_test.bmp";
    int aa = 1;

    for (int i = 1; i < argc - 1; ++i)
    {
        if (!strcmp(argv[i], "--fnt")) fnt = argv[++i];
        else if (!strcmp(argv[i], "--out")) out = argv[++i];
        else if (!strcmp(argv[i], "--aa")) aa = atoi(argv[++i]);
    }

    WriteIni(aa);
    if (!LoadGameFnt(fnt)) { printf("[x] cannot load %s\n", fnt); return 1; }
    printf("[*] game.fnt: lines=%d symbolSize=%d glyphs=%d\n", g_lines, g_symbolSize, g_glyphCount);

    FakeInternal in;
    memset(&in, 0, sizeof(in));
    in.stride = 3;
    in.lines = g_lines;
    in.symbolBytes = (unsigned int)g_symbolSize;
    in.symTable = g_map;
    in.bitmaps = g_bitmaps;

    for (int i = 0; i < W * H; ++i) g_surf[i] = BG;
    InitBitFont(&in, g_surf, W, FG, 0, 0, W - 1, H - 1);

    if (!vt::Takeover::Init())
    {
        printf("[x] Takeover::Init failed (font not loaded)\n");
        return 2;
    }
    printf("[*] takeover ready, aa=%d\n\n", aa);

    // ---- the test string, with the engine's own advances --------------------
    static wchar_t text[256];
    wcscpy_s(text, L"Phobos \u4E2D\u6587\u6D4B\u8BD5 0123 ok");
    unsigned int cps[256]; int n = 0;
    for (const wchar_t* p = text; *p && n < 256; ++p) cps[n++] = (unsigned int)*p;

    printf("1) layout: pen X must advance by the ORIGINAL game.fnt width\n");
    {
        int x = 4, mismatches = 0, drawn = 0;
        for (int i = 0; i < n; ++i)
        {
            int newX = -1;
            const bool ok = vt::Takeover::TryBlit(g_bitFont, cps[i], x, 4, -1, &newX);
            const int expect = AdvOf(cps[i]);
            if (ok)
            {
                ++drawn;
                if (newX - x != expect) { ++mismatches; printf("     U+%04X: got %d expected %d\n", cps[i], newX - x, expect); }
                x = newX;
            }
            else if (expect > 0)
            {
                ++mismatches;
                printf("     U+%04X: takeover refused (engine would draw it)\n", cps[i]);
            }
        }
        CHECK(mismatches == 0 && drawn >= n - 1, "drew %d/%d glyphs, advance mismatches=%d", drawn, n, mismatches);
        printf("     final pen X = %d\n", x);
    }

    printf("\n2) pixels: something must be on the surface\n");
    const int ink1 = CountInk();
    CHECK(ink1 > 100, "ink pixels = %d", ink1);

    printf("\n3) colour: from BitFont+0x24 when arg is -1, from the arg otherwise\n");
    {
        // paint a 4x4 block of the surface, then draw one glyph with an explicit colour
        for (int i = 0; i < W * H; ++i) g_surf[i] = BG;
        int newX = 0;
        const unsigned short other = 0x07E0;             // pure green
        vt::Takeover::TryBlit(g_bitFont, (unsigned)text[0], 4, 4, other, &newX);
        int found = 0, pure = 0;
        bool onlyGreen = true;
        for (int y = 4; y < 4 + g_lines; ++y)
            for (int x = 4; x < 40; ++x)
            {
                const unsigned short c = g_surf[y * W + x];
                if (c == BG) continue;
                ++found;
                if (c == other) ++pure;
                if ((c & 0xF81F) || !(c & 0x07E0)) onlyGreen = false;
            }
        // A thin AA glyph need not contain any fully covered pixel. Verify
        // the requested colour channels, rather than assuming an opaque core.
        CHECK(found > 0 && onlyGreen && (aa || pure == found),
              "arg colour used: %d lit, %d exactly 0x%04X (aa=%d)", found, pure, other, aa);
    }

    printf("\n4) AA: edge pixels are blended, not pure colour\n");
    {
        for (int i = 0; i < W * H; ++i) g_surf[i] = BG;
        int newX = 0;
        vt::Takeover::TryBlit(g_bitFont, (unsigned)L'\u4E2D', 4, 4, -1, &newX);
        int partial = 0, full = 0;
        for (int i = 0; i < W * H; ++i)
        {
            if (g_surf[i] == BG) continue;
            if (g_surf[i] == FG) ++full; else ++partial;
        }
        if (aa)
            CHECK(partial > 0, "partial-coverage pixels = %d (full = %d)", partial, full);
        else
            CHECK(partial == 0, "mono mode: partial = %d (must be 0), full = %d", partial, full);
    }

    printf("\n5) clipping to BitFont bounds\n");
    {
        for (int i = 0; i < W * H; ++i) g_surf[i] = BG;
        InitBitFont(&in, g_surf, W, FG, 0, 0, 19, 19);      // 20x20 clip box at the origin
        int newX = 0;
        vt::Takeover::TryBlit(g_bitFont, (unsigned)L'\u4E2D', 10, 10, -1, &newX);
        bool outside = false;
        for (int y = 0; y < H && !outside; ++y)
            for (int x = 0; x < W; ++x)
                if (g_surf[y * W + x] != BG && (x > 19 || y > 19)) { outside = true; break; }
        CHECK(!outside, "nothing written outside the 0,0..19,19 bounds (ink=%d)", CountInk());
        InitBitFont(&in, g_surf, W, FG, 0, 0, W - 1, H - 1);
    }

    printf("\n6) unlocked surface is refused (engine handles it)\n");
    {
        memset(g_bitFont, 0, sizeof(g_bitFont));
        *(void**)(g_bitFont + 0x04) = &in;
        int newX = -1;
        const bool ok = vt::Takeover::TryBlit(g_bitFont, (unsigned)'A', 4, 4, -1, &newX);
        CHECK(!ok, "TryBlit returned %d for an unlocked surface", (int)ok);
    }

    printf("\n7) characters with no game.fnt glyph are refused\n");
    {
        InitBitFont(&in, g_surf, W, FG, 0, 0, W - 1, H - 1);
        unsigned int missing = 0;
        for (unsigned int cp = 0x0400; cp < 0x0500; ++cp)      // Cyrillic: unmapped in game.fnt
            if (cp < 0x10000 && g_map[cp] == 0) { missing = cp; break; }
        int newX = -1;
        const bool ok = missing ? vt::Takeover::TryBlit(g_bitFont, missing, 4, 4, -1, &newX) : false;
        CHECK(missing != 0 && !ok, "U+%04X refused (engine draws its placeholder)", missing);
    }

    printf("\n8) tracking agrees with native drawn and clipped return paths\n");
    {
        InitBitFont(&in, g_surf, W, FG, 0, 0, W - 1, H - 1);
        bool matched = true;
        for (int extra : {0, 1, 3, -1})
        {
            *(int*)(g_bitFont + 0x2C) = extra;
            int pen = -1;
            matched = matched && vt::Takeover::TryBlit(g_bitFont, 'A', 4, 4, -1, &pen) &&
                pen == 4 + AdvOf('A') + extra;
            // A valid clip but an offscreen glyph must return the same advance.
            matched = matched && vt::Takeover::TryBlit(g_bitFont, 'A', -100, 4, -1, &pen) &&
                pen == -100 + AdvOf('A') + extra;
        }
        CHECK(matched, "native X + width + tracking preserved, including clipped glyphs");
    }
    printf("\n9) probe rectangle is valid; actual empty clips are refused without mutation\n");
    {
        unsigned short* large = (unsigned short*)calloc(1920 * 580, sizeof(unsigned short));
        InitBitFont(&in, large, 1920, FG, 1100, 550, 1300, 570);
        int pen = -1;
        CHECK(large && vt::Takeover::TryBlit(g_bitFont, 0x7F8E, 1270, 550, -1, &pen),
              "logged 1100,550,1300,570 rectangle accepted");
        free(large);
        InitBitFont(&in, g_surf, W, FG, 0, 0, -1, -1);
        memset(g_surf, 0, sizeof(g_surf));
        const unsigned char widthBefore = g_bitmaps[(g_map['A'] - 1) * g_symbolSize];
        bool refused = true;
        for (int i = 0; i < 10000; ++i)
            refused = refused && !vt::Takeover::TryBlit(g_bitFont, 'A', 4, 4, -1, &pen);
        CHECK(refused && !CountInk() &&
              g_bitmaps[(g_map['A'] - 1) * g_symbolSize] == widthBefore,
              "empty-clip refusals leave pixels and glyph widths untouched");
        InitBitFont(&in, g_surf, W, FG, W + 4, 0, W + 20, 19);
        CHECK(!vt::Takeover::TryBlit(g_bitFont, 'A', W + 4, 4, -1, &pen),
              "valid raw rectangle beyond surface pitch is safely refused");
        InitBitFont(&in, g_surf, W, FG, 0, 0, W - 1, H - 1);
    }

    // ---- hot path throughput ----------------------------------------------
    printf("\n10) hot path throughput (a session draws ~775k glyphs)\n");
    {
        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        const int N = 200000;
        int x = 4;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < N; ++i)
        {
            int newX = -1;
            if (vt::Takeover::TryBlit(g_bitFont, cps[i % n], x, 4, -1, &newX))
                x = (newX < 200) ? newX : 4;
        }
        QueryPerformanceCounter(&t1);
        const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        printf("  [info] %d glyphs in %.1f ms -> %.1f glyphs/ms", N, ms, (double)N / ms);
        if (ms > 0.0)
            printf("  (a 775k-glyph session would cost ~%.0f ms)", ms * 775000.0 / N);
        printf("\n");
        CHECK(ms < 2000.0, "throughput acceptable (%.1f ms for %d glyphs)", ms, N);
    }

    // ---- final picture -----------------------------------------------------
    for (int i = 0; i < W * H; ++i) g_surf[i] = BG;
    InitBitFont(&in, g_surf, W, FG, 0, 0, W - 1, H - 1);
    {
        int x = 4;
        for (int i = 0; i < n; ++i)
        {
            int newX = -1;
            if (vt::Takeover::TryBlit(g_bitFont, cps[i], x, 4, -1, &newX))
                x = newX;
            else
                x += (AdvOf(cps[i]) > 0 ? AdvOf(cps[i]) : 6);
        }
    }
    printf("\n   final render:\n");
    DumpArt(2, 20);
    WriteBmp(out);

    unsigned long long drawn = 0, skipped = 0, failed = 0, unknown = 0;
    vt::Takeover::Stats(&drawn, &skipped, &failed, &unknown);
    printf("\n[*] stats: drawn=%llu failed=%llu unknown=%llu -> %s\n",
           drawn, failed, unknown, out);
    printf("[%s] %d check(s) failed\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 3 : 0;
}
