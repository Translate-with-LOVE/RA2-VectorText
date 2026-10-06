// ===========================================================================
//  glyph_test.cpp -- offline check that our rasterised glyph cells land where
//  the game's own bitmap glyphs land.
//
//  For each character it prints, side by side:
//      game.fnt  : the original 1bpp cell (what the engine draws today)
//      GlyphSource: our FreeType cell (Noto Serif SC, engine cell metrics)
//  and the ink row range of both, so the baseline can be tuned offline.
//
//  usage: glyph_test.exe [--ttf <path>] [--size N] [--wght N] [--baseline N]
//                         [--chars "<utf8 text>"] [--fnt <game.fnt>]
// ===========================================================================

#include "../src/GlyphSource.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wchar.h>

// ---------------------------------------------------------------- game.fnt
static unsigned char* g_fntData = NULL;
static unsigned short* g_fntMap = NULL;
static int g_stride = 3, g_lines = 16, g_symbolSize = 49, g_glyphOff = 0;

static bool LoadGameFnt(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_fntData = (unsigned char*)malloc(size);
    if (!g_fntData) { fclose(f); return false; }
    if (fread(g_fntData, 1, size, f) != (size_t)size) { fclose(f); return false; }
    fclose(f);

    if (*(unsigned int*)g_fntData != 0x546E6F66u)      // 'fonT'
        return false;

    const unsigned int* hdr = (const unsigned int*)(g_fntData + 4);
    g_stride     = (int)hdr[1];
    g_lines      = (int)hdr[2];
    g_symbolSize = (int)hdr[5];
    g_fntMap     = (unsigned short*)(g_fntData + 0x1C);
    g_glyphOff   = 0x1C + 0x20000;
    return true;
}

static const unsigned char* GameGlyph(unsigned int cp, int* width)
{
    if (!g_fntData || cp >= 0x10000) return NULL;
    const unsigned short idx = g_fntMap[cp];
    if (!idx) return NULL;
    const unsigned char* p = g_fntData + g_glyphOff + (size_t)(idx - 1) * g_symbolSize;
    *width = p[0];
    return p + 1;
}

static void ArtRows(const unsigned char* rows, int stride, int from, int to,
                    char out[][64])
{
    for (int r = from; r <= to; ++r)
    {
        int n = 0;
        for (int b = 0; b < stride; ++b)
        {
            const unsigned char byte = rows[r * stride + b];
            for (int k = 7; k >= 0; --k)
                out[r - from][n++] = (byte >> k) & 1 ? '#' : '.';
        }
        out[r - from][n] = 0;
    }
}

static void InkRange(const unsigned char* rows, int stride, int lines, int* first, int* last)
{
    *first = -1; *last = -1;
    for (int r = 0; r < lines; ++r)
    {
        bool any = false;
        for (int b = 0; b < stride && !any; ++b)
            if (rows[r * stride + b]) any = true;
        if (any) { if (*first < 0) *first = r; *last = r; }
    }
}

static void InkCols(const unsigned char* rows, int stride, int lines, int* first, int* last)
{
    *first = -1; *last = -1;
    for (int r = 0; r < lines; ++r)
        for (int x = 0; x < stride * 8; ++x)
            if (rows[r * stride + (x >> 3)] & (0x80 >> (x & 7)))
            {
                if (*first < 0 || x < *first) *first = x;
                if (x > *last) *last = x;
            }
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    const char* ttf   = "C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf";
    const char* fnt   = "..\\..\\..\\game.fnt";      // build\test -> workspace -> game dir
    int size = 13, wght = 400, baseline = 13, cjkSize = 0;   // cjkSize=0 -> same as size
    bool summary = false, nofit = false;
    static wchar_t chars[256];
    // wide literal: MSVC encodes \uXXXX escapes as UTF-16 regardless of the
    // system code page (a narrow literal here becomes GBK on this machine)
    wcscpy_s(chars, L"Aa0g\u4E2D\u6587\uFF0C\u3002\u7535\u529B");

    for (int i = 1; i < argc - 1; ++i)
    {
        if (!strcmp(argv[i], "--ttf"))      ttf = argv[++i];
        else if (!strcmp(argv[i], "--size"))     size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wght"))     wght = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--baseline")) baseline = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sizes"))
        {
            int l = size, c = 0;
            if (sscanf_s(argv[++i], "%d,%d", &l, &c) >= 1) { size = l; cjkSize = c; }
        }
        else if (!strcmp(argv[i], "--summary")) summary = true;
        else if (!strcmp(argv[i], "--nofit")) nofit = true;
        else if (!strcmp(argv[i], "--fnt"))      fnt = argv[++i];
        else if (!strcmp(argv[i], "--chars"))
        {
            static char utf8[512];
            strncpy_s(utf8, argv[++i], _TRUNCATE);
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, chars, 256);
        }
    }

    if (!LoadGameFnt(fnt))
    {
        printf("[x] cannot load %s\n", fnt);
        return 1;
    }
    printf("[*] game.fnt : stride=%d lines=%d symbolSize=%d\n", g_stride, g_lines, g_symbolSize);

    vt::GlyphSource src;
    if (!src.Init(ttf, size, wght, g_stride, g_lines, baseline))
    {
        printf("[x] GlyphSource::Init failed (%s @ %dpx)\n", ttf, size);
        return 2;
    }
    if (nofit) src.SetFitToAdvance(false);
    if (cjkSize > 0) src.SetSizes(size, cjkSize);
    printf("[*] vector   : %s @ %dpx (cjk %dpx) wght=%d baselineRow=%d fit=%d\n\n",
           ttf, size, cjkSize > 0 ? cjkSize : size, wght, baseline, nofit ? 0 : 1);

    long long inkFnt = 0, inkOurs = 0, covOurs = 0;
    for (const wchar_t* p = chars; *p; ++p)
    {
        const unsigned int cp = (unsigned int)*p;
        int fntWidth = 0;
        const unsigned char* fntRows = GameGlyph(cp, &fntWidth);
        const vt::GlyphCell* cell = src.Get(cp, fntWidth > 0 ? fntWidth : -1);

        int fFirst = -1, fLast = -1, vFirst = -1, vLast = -1;
        if (fntRows) InkRange(fntRows, g_stride, g_lines, &fFirst, &fLast);
        if (cell)    InkRange(cell->bits, g_stride, g_lines, &vFirst, &vLast);

        int fc0 = -1, fc1 = -1, vc0 = -1, vc1 = -1;
        if (fntRows) InkCols(fntRows, g_stride, g_lines, &fc0, &fc1);
        if (cell)    InkCols(cell->bits, g_stride, g_lines, &vc0, &vc1);

        // raw FreeType metrics **from the face GlyphSource itself uses**
        // (a fresh face here would not have the wght=400 variation applied)
        {
            FT_Face face = (FT_Face)src.FaceHandle();
            if (face && !FT_Load_Char(face, cp, FT_LOAD_TARGET_MONO | FT_LOAD_RENDER))
            {
                const FT_GlyphSlot g = face->glyph;
                printf("        ft(same face): size=%d left=%d top=%d w=%u rows=%u mode=%d pitch=%d adv=%ld\n",
                       size, g->bitmap_left, g->bitmap_top, g->bitmap.width, g->bitmap.rows,
                       g->bitmap.pixel_mode, g->bitmap.pitch, (long)(g->advance.x >> 6));
            }
        }

        printf("U+%04X  fnt w=%-3d rows %2d..%2d cols %2d..%2d   ours w=%-3d rows %2d..%2d cols %2d..%2d\n",
               cp, fntWidth, fFirst, fLast, fc0, fc1,
               cell ? cell->width : -1, vFirst, vLast, vc0, vc1);

        if (fntRows)
            for (int r = 0; r < g_lines; ++r)
                for (int b = 0; b < g_stride; ++b)
                    for (int k = 0; k < 8; ++k)
                        if (fntRows[r * g_stride + b] & (0x80 >> k)) ++inkFnt;
        if (cell)
        {
            for (int r = 0; r < g_lines; ++r)
                for (int b = 0; b < g_stride; ++b)
                    for (int k = 0; k < 8; ++k)
                        if (cell->bits[r * g_stride + b] & (0x80 >> k)) ++inkOurs;
            for (int r = 0; r < g_lines; ++r)
                for (int x = 0; x < 24; ++x) covOurs += cell->cov[r * 24 + x];
        }

        if (summary) { printf("\n"); continue; }
        char a[40][64], b[40][64];
        const int rows = g_lines;
        if (fntRows) ArtRows(fntRows, g_stride, 0, rows - 1, a);
        if (cell)    ArtRows(cell->bits, g_stride, 0, rows - 1, b);

        for (int r = 0; r < rows; ++r)
        {
            printf("   %-26s | %-26s\n",
                   fntRows ? a[r] : "", cell ? b[r] : "");
        }
        printf("\n");
    }

    printf("[*] cached glyphs: %u\n", (unsigned)src.CachedGlyphs());
    return 0;
}
