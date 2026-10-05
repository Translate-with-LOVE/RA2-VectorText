// ===========================================================================
//  ft_smoke.cpp -- prove the statically linked 32-bit FreeType works, and show
//  what M1 will actually get out of it:
//
//    * FreeType version and the face's family name / variable-font flag
//    * per-character 1bpp rasterisation (FT_LOAD_TARGET_MONO) -> what M1 writes
//    * per-character 8bpp coverage   (FT_LOAD_TARGET_NORMAL) -> what M2 blends
//    * advance / bearing, i.e. the metrics M1 needs for Metrics=freetype
//
//  usage: ft_smoke.exe [--face <ttf>] [--size N] [--chars "<text>"]
// ===========================================================================

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void printMono(const FT_Bitmap& bm)
{
    for (unsigned int r = 0; r < bm.rows; ++r)
    {
        const unsigned char* row = bm.buffer + r * bm.pitch;
        printf("      ");
        for (unsigned int c = 0; c < bm.width; ++c)
        {
            const unsigned char byte = row[c >> 3];
            const int bit = (byte >> (7 - (c & 7))) & 1;
            putchar(bit ? '#' : '.');
        }
        putchar('\n');
    }
}

static void measureGray(const FT_Bitmap& bm, int* maxCoverage, long* inkPixels, long* softPixels)
{
    *maxCoverage = 0; *inkPixels = 0; *softPixels = 0;
    for (unsigned int r = 0; r < bm.rows; ++r)
    {
        const unsigned char* row = bm.buffer + r * bm.pitch;
        for (unsigned int c = 0; c < bm.width; ++c)
        {
            const int v = row[c];
            if (v > *maxCoverage) *maxCoverage = v;
            if (v >= 128) ++*inkPixels;
            else if (v > 0) ++*softPixels;
        }
    }
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    const char* facePath = "C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf";
    int size = 13;
    long wght = 400;                 // NotoSerifSC-VF's default instance is ExtraLight
    const wchar_t* chars = L"Aa0\u4E2D\u6587\u6E38\u620F\uFF01";

    for (int i = 1; i < argc - 1; ++i)
    {
        if (!strcmp(argv[i], "--face"))       facePath = argv[++i];
        else if (!strcmp(argv[i], "--size"))  size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wght"))  wght = atol(argv[++i]);
        else if (!strcmp(argv[i], "--chars"))
        {
            // convert the UTF-8 argument to wide chars
            static wchar_t wide[256];
            MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, wide, 256);
            chars = wide;
        }
    }

    FT_Library lib = NULL;
    if (FT_Init_FreeType(&lib))
    {
        printf("[x] FT_Init_FreeType failed\n");
        return 1;
    }

    FT_Int maj = 0, min = 0, pat = 0;
    FT_Library_Version(lib, &maj, &min, &pat);
    printf("[*] FreeType %d.%d.%d (statically linked, 32-bit)\n", maj, min, pat);

    FT_Face face = NULL;
    if (FT_New_Face(lib, facePath, 0, &face))
    {
        printf("[x] cannot open face: %s\n", facePath);
        FT_Done_FreeType(lib);
        return 2;
    }
    printf("[*] face       : %s / %s  (%ld glyphs)\n",
           face->family_name ? face->family_name : "?",
           face->style_name ? face->style_name : "?", (long)face->num_glyphs);
    printf("[*] variable   : %s\n", FT_HAS_MULTIPLE_MASTERS(face) ? "yes" : "no");
    printf("[*] units/em   : %d   fixed sizes: %d\n", face->units_per_EM, face->num_fixed_sizes);

    // ---- variable font: pick a real weight ---------------------------------
    // NotoSerifSC-VF's default instance is ExtraLight, which is unusable for a
    // game HUD; M1 must select Regular (wght = 400) explicitly.
    if (FT_HAS_MULTIPLE_MASTERS(face))
    {
        FT_MM_Var* mm = NULL;
        if (!FT_Get_MM_Var(face, &mm))
        {
            printf("[*] axes       : %u  named styles: %u\n", mm->num_axis, mm->num_namedstyles);
            for (FT_UInt a = 0; a < mm->num_axis; ++a)
            {
                const FT_Var_Axis& ax = mm->axis[a];
                char tag[5] = { (char)(ax.tag >> 24), (char)(ax.tag >> 16), (char)(ax.tag >> 8), (char)ax.tag, 0 };
                printf("       %s  min=%ld def=%ld max=%ld\n", tag,
                       (long)(ax.minimum >> 16), (long)(ax.def >> 16), (long)(ax.maximum >> 16));
            }
            if (wght > 0)
            {
                FT_Fixed* coords = (FT_Fixed*)malloc(sizeof(FT_Fixed) * mm->num_axis);
                for (FT_UInt a = 0; a < mm->num_axis; ++a)
                    coords[a] = mm->axis[a].def;
                for (FT_UInt a = 0; a < mm->num_axis; ++a)
                {
                    char tag[5] = { (char)(mm->axis[a].tag >> 24), (char)(mm->axis[a].tag >> 16),
                                    (char)(mm->axis[a].tag >> 8), (char)mm->axis[a].tag, 0 };
                    if (!strcmp(tag, "wght"))
                        coords[a] = wght << 16;
                }
                const FT_Error e = FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords);
                printf("[*] wght       : requested %ld -> %s\n", wght, e ? "FAILED" : "applied");
                free(coords);
            }
            FT_Done_MM_Var(lib, mm);
        }
    }

    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size))
    {
        printf("[x] FT_Set_Pixel_Sizes(%d) failed\n", size);
        FT_Done_Face(face);
        FT_Done_FreeType(lib);
        return 3;
    }
    printf("[*] pixel size : %d  (ascender %ld, descender %ld, height %ld)\n",
           size, (long)(face->size->metrics.ascender >> 6),
           (long)(face->size->metrics.descender >> 6),
           (long)(face->size->metrics.height >> 6));
    printf("\n");

    int ok = 0;
    for (const wchar_t* p = chars; *p; ++p)
    {
        const FT_ULong cp = (FT_ULong)*p;

        // ---- 1bpp: what M1 writes into the game's 8bpp surface -------------
        if (FT_Load_Char(face, cp, FT_LOAD_TARGET_MONO | FT_LOAD_RENDER))
        {
            printf("  U+%04X  <load failed>\n", (unsigned)cp);
            continue;
        }
        const FT_GlyphSlot m = face->glyph;
        printf("  U+%04X  advance=%ld  bearing=(%d,%d)  mono %ux%u\n",
               (unsigned)cp, (long)(m->advance.x >> 6), m->bitmap_left, m->bitmap_top,
               m->bitmap.width, m->bitmap.rows);
        if (m->bitmap.pixel_mode == FT_PIXEL_MODE_MONO && m->bitmap.rows)
            printMono(m->bitmap);

        // ---- 8bpp coverage: what M2 blends ---------------------------------
        if (!FT_Load_Char(face, cp, FT_LOAD_TARGET_NORMAL | FT_LOAD_RENDER))
        {
            const FT_GlyphSlot g = face->glyph;
            int maxc = 0; long ink = 0, soft = 0;
            measureGray(g->bitmap, &maxc, &ink, &soft);
            printf("           coverage: max=%d  solid=%ld  soft=%ld\n", maxc, ink, soft);
        }
        ++ok;
    }

    printf("\n[*] %d characters rendered -- static FreeType is usable from this build.\n", ok);

    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return ok ? 0 : 4;
}
