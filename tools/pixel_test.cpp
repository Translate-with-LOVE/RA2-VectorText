// ===========================================================================
//  pixel_test.cpp -- offline proof that the M1 pixel writer produces what the
//  engine's own Blit would produce, on a synthetic 16-bit surface:
//
//     row 1 : the ORIGINAL game.fnt glyphs, blitted with our writer
//     row 2 : OUR vector glyphs, same layout / colour / formula
//     row 3 : same as row 2 but with the engine's `reveal` colour ramp
//
//  Also writes pixel_test.bmp so the result can be looked at directly.
//  usage: pixel_test.exe [--sizes L,C] [--baseline N] [--out file.bmp]
// ===========================================================================

#include "../src/GlyphSource.h"
#include "../src/PixelWriter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wchar.h>

// ------------------------------------------------------------- game.fnt ---
static unsigned char* g_data = NULL;
static unsigned short* g_map = NULL;
static int g_stride = 3, g_lines = 16, g_symbolSize = 49, g_glyphOff = 0;

static bool LoadGameFnt(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
    g_data = (unsigned char*)malloc(size);
    if (!g_data || fread(g_data, 1, size, f) != (size_t)size) { fclose(f); return false; }
    fclose(f);
    if (*(unsigned int*)g_data != 0x546E6F66u) return false;
    const unsigned int* hdr = (const unsigned int*)(g_data + 4);
    g_stride = (int)hdr[1]; g_lines = (int)hdr[2]; g_symbolSize = (int)hdr[5];
    g_map = (unsigned short*)(g_data + 0x1C);
    g_glyphOff = 0x1C + 0x20000;
    return true;
}

// original cell, in the same struct our rasteriser produces
static bool GameCell(unsigned int cp, vt::GlyphCell* out)
{
    if (!g_data || cp >= 0x10000 || g_stride != 3) return false;
    const unsigned short idx = g_map[cp];
    if (!idx) return false;
    const unsigned char* p = g_data + g_glyphOff + (size_t)(idx - 1) * g_symbolSize;
    memset(out, 0, sizeof(*out));
    out->width = p[0];
    memcpy(out->bits, p + 1, g_stride * g_lines);
    return true;
}

// ---------------------------------------------------------------- surface --
static const int W = 400, H = 96;
static unsigned short g_surf[W * H];
static const unsigned short BG = 0x0000;            // black panel
static const unsigned short FG = 0xC618;            // RGB565 light grey

static vt::Target MakeTarget()
{
    vt::Target t;
    t.base = g_surf;
    t.pitch = W;
    t.clipL = 0; t.clipT = 0; t.clipR = W - 1; t.clipB = H - 1;
    return t;
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
    *(int*)(img + 2) = fileSize;
    *(int*)(img + 10) = 54;
    *(int*)(img + 14) = 40;
    *(int*)(img + 18) = W;
    *(int*)(img + 22) = H;
    *(short*)(img + 26) = 1;
    *(short*)(img + 28) = 24;
    *(int*)(img + 34) = dataSize;

    for (int y = 0; y < H; ++y)
    {
        unsigned char* dst = img + 54 + (size_t)(H - 1 - y) * (rowBytes + pad);
        for (int x = 0; x < W; ++x)
        {
            const unsigned short c = g_surf[y * W + x];
            const int r = ((c >> 11) & 0x1F) * 255 / 31;
            const int g = ((c >> 5) & 0x3F) * 255 / 63;
            const int b = (c & 0x1F) * 255 / 31;
            dst[x * 3 + 0] = (unsigned char)b;
            dst[x * 3 + 1] = (unsigned char)g;
            dst[x * 3 + 2] = (unsigned char)r;
        }
    }
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(img, 1, fileSize, f); fclose(f); }
    free(img);
}

static void DumpArt(int y0, int y1)
{
    for (int y = y0; y <= y1; ++y)
    {
        printf("   ");
        for (int x = 0; x < 150; ++x)
            putchar(g_surf[y * W + x] != BG ? '#' : '.');
        printf("\n");
    }
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    const char* ttf = "C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf";
    const char* fnt = "..\\..\\..\\game.fnt";
    const char* out = "pixel_test.bmp";
    int latin = 13, cjk = 16, baseline = 13;

    static wchar_t text[256];
    wcscpy_s(text, L"Phobos \u4E2D\u6587\u6D4B\u8BD5 0123 \u7535\u529B=2160 ok");

    for (int i = 1; i < argc - 1; ++i)
    {
        if (!strcmp(argv[i], "--ttf"))      ttf = argv[++i];
        else if (!strcmp(argv[i], "--fnt")) fnt = argv[++i];
        else if (!strcmp(argv[i], "--out")) out = argv[++i];
        else if (!strcmp(argv[i], "--baseline")) baseline = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sizes"))
        {
            int l = latin, c = cjk;
            if (sscanf_s(argv[++i], "%d,%d", &l, &c) >= 1) { latin = l; cjk = c; }
        }
    }

    if (!LoadGameFnt(fnt)) { printf("[x] cannot load %s\n", fnt); return 1; }

    vt::GlyphSource src;
    if (!src.Init(ttf, latin, 400, g_stride, g_lines, baseline))
    {
        printf("[x] GlyphSource::Init failed\n");
        return 2;
    }
    src.SetSizes(latin, cjk);

    // codepoints + the engine's advances (Metrics=game)
    unsigned int cps[256]; int adv[256]; int n = 0;
    for (const wchar_t* p = text; *p && n < 256; ++p)
    {
        vt::GlyphCell gc;
        cps[n] = (unsigned int)*p;
        adv[n] = GameCell(cps[n], &gc) ? gc.width : -1;
        ++n;
    }
    printf("[*] %d codepoints, advance total (engine) = ", n);
    int total = 0;
    for (int i = 0; i < n; ++i) total += (adv[i] > 0 ? adv[i] : 0);
    printf("%d px\n", total);

    for (int i = 0; i < W * H; ++i) g_surf[i] = BG;
    vt::Target t = MakeTarget();

    // ---- row 1: the ORIGINAL bitmap glyphs, drawn with our pixel writer -----
    {
        int x = 4;
        for (int i = 0; i < n; ++i)
        {
            vt::GlyphCell gc;
            if (GameCell(cps[i], &gc))
            {
                vt::DrawCell(t, gc, x, 2, g_lines, FG);
                x += gc.width;
            }
        }
    }

    // ---- row 2: our vector glyphs, same layout/colour ----------------------
    {
        vt::Target t2 = t;
        t2.clipT = 22; t2.clipB = 22 + g_lines - 1;
        vt::DrawString(t2, src, cps, adv, n, 4, 22, g_lines, FG, 0);
    }

    // ---- row 3: engine `reveal` colour ramp (1..8) -------------------------
    {
        vt::Target t3 = t;
        t3.clipT = 46; t3.clipB = 46 + g_lines - 1;
        vt::DrawString(t3, src, cps, adv, n, 4, 46, g_lines, FG, 4);
    }

    // ---- row 4: a solid bar to show the un-blended colour ------------------
    for (int x = 4; x < 160; ++x)
        g_surf[72 * W + x] = FG;

    printf("\n   row1 = game.fnt glyphs   row2 = vector glyphs   row3 = reveal=4 ramp\n");
    printf("   row4 = solid colour reference\n\n");
    DumpArt(0, 78);

    WriteBmp(out);
    int lit = 0;
    for (int i = 0; i < W * H; ++i) if (g_surf[i] != BG) ++lit;
    printf("\n[*] written %s  (%dx%d, %d lit pixels)\n", out, W, H, lit);
    return 0;
}
