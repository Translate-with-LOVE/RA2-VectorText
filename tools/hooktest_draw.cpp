// ===========================================================================
//  hooktest_draw.cpp -- the *hook glue* under test, in draw mode.
//
//  takeover_test.cpp proves that Takeover::TryBlit draws correctly; this test
//  proves the layer above it: that the exported hook handler, when driven
//  exactly the way Syringe drives it (REGISTERS block, ESP at the return
//  address), implements the "skip the callee" contract properly:
//
//     * returns the caller's return address (so Syringe jumps there)
//     * pops the return address + the 4 stack arguments (ret 0x10)
//     * leaves the new pen X in EAX
//     * has actually written the glyph into the locked 16-bit surface
//
//  and, in the refusal cases, that it returns 0 with ESP untouched (the engine
//  then runs its own code, i.e. a byte-for-byte fallback).
//
//  usage: hooktest_draw.exe [--mode draw|observe] [--fnt path]
// ===========================================================================

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct REGISTERS
{
    DWORD origin, flags, edi, esi, ebp, esp, ebx, edx, ecx, eax;
};
typedef DWORD(__cdecl* HookProc)(REGISTERS*);

static int g_fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fails; printf("  [FAIL] "); } else printf("  [ ok ] "); \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

// ------------------------------------------------------------- game.fnt ----
static unsigned char* g_fnt = NULL;
static int g_lines = 16, g_symbolSize = 49;
static unsigned short* g_map = NULL;
static const unsigned char* g_bitmaps = NULL;

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
    g_map = (unsigned short*)(g_fnt + 0x1C);
    g_bitmaps = g_fnt + 0x1C + 0x20000;
    return true;
}

static int GameAdvance(unsigned int cp)
{
    if (cp >= 0x10000 || !g_map[cp]) return -1;
    return g_bitmaps[(size_t)(g_map[cp] - 1) * g_symbolSize];
}

// ------------------------------------------------------ synthetic objects --
#pragma pack(push, 1)
struct FakeInternal
{
    unsigned int  unknown00, stride, lines, unknown0C, unknown10, symbolBytes;
    const unsigned short* symTable;
    const unsigned char*  bitmaps;
};
#pragma pack(pop)

static const int W = 320, H = 48;
static unsigned short g_surf[W * H];
static unsigned char  g_bitFont[0x80];
static FakeInternal   g_internal;
static const unsigned short BG = 0x0000, FG = 0xC618;

static void InitObjects()
{
    memset(&g_internal, 0, sizeof(g_internal));
    g_internal.stride = 3;
    g_internal.lines = g_lines;
    g_internal.symbolBytes = (unsigned int)g_symbolSize;
    g_internal.symTable = g_map;
    g_internal.bitmaps = g_bitmaps;

    memset(g_bitFont, 0, sizeof(g_bitFont));
    *(void**)(g_bitFont + 0x04) = &g_internal;
    *(void**)(g_bitFont + 0x0C) = g_surf;
    *(int*)(g_bitFont + 0x10) = W;
    *(unsigned short*)(g_bitFont + 0x24) = FG;
    *(int*)(g_bitFont + 0x30) = 0;
    *(int*)(g_bitFont + 0x34) = 0;
    *(int*)(g_bitFont + 0x38) = W - 1;
    *(int*)(g_bitFont + 0x3C) = H - 1;
}

static int InkCount()
{
    int n = 0;
    for (int i = 0; i < W * H; ++i) if (g_surf[i] != BG) ++n;
    return n;
}

static void WriteIni(const char* mode)
{
    FILE* f = fopen("VectorText.ini", "w");
    if (!f) return;
    fprintf(f, "[VectorText]\nEnabled=1\nMode=%s\nAntiAlias=1\n"
               "FontFile=C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf\n"
               "FontWeight=400\nFontSizeLatin=13\nFontSizeCJK=16\nBaselineRow=13\nFitToAdvance=1\n",
            mode);
    fclose(f);
}

// Simulate the game calling BitFont::Blit(ch, x, y, color): the hook sees
// ESP -> [ret][ch][x][y][color] and ECX = the BitFont object.
struct BlitCall
{
    DWORD ret;
    DWORD ch, x, y, color;
    DWORD pad;                       // room for the handler to peek
};

static DWORD DriveBlit(HookProc proc, unsigned int ch, int x, int y, int color,
                       BlitCall* outCall, REGISTERS* outRegs, DWORD* outEsp)
{
    BlitCall call;
    call.ret = 0x00401234;
    call.ch = ch;
    call.x = (DWORD)x;
    call.y = (DWORD)y;
    call.color = (DWORD)color;

    REGISTERS r;
    memset(&r, 0, sizeof(r));
    r.origin = 0x00434120;
    r.esp = (DWORD)(UINT_PTR)&call;
    r.ecx = (DWORD)(UINT_PTR)g_bitFont;

    if (outEsp) *outEsp = r.esp;
    const DWORD ret = proc(&r);
    if (outCall) *outCall = call;
    if (outRegs) *outRegs = r;
    return ret;
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    const char* mode = "draw";
    const char* fnt = "..\\..\\..\\game.fnt";
    for (int i = 1; i < argc - 1; ++i)
    {
        if (!strcmp(argv[i], "--mode")) mode = argv[++i];
        else if (!strcmp(argv[i], "--fnt")) fnt = argv[++i];
    }

    WriteIni(mode);
    if (!LoadGameFnt(fnt)) { printf("[x] cannot load %s\n", fnt); return 1; }
    printf("[*] mode=%s  game.fnt lines=%d\n", mode, g_lines);

    HMODULE dll = LoadLibraryA("VectorText.dll");
    if (!dll) { printf("[x] LoadLibraryA failed: %lu\n", GetLastError()); return 2; }
    HookProc blit = (HookProc)GetProcAddress(dll, "VT_Hook_BitFont_Blit");
    if (!blit) { printf("[x] export missing\n"); return 3; }

    for (int i = 0; i < W * H; ++i) g_surf[i] = BG;
    InitObjects();

    // snapshot the original glyph bytes *before* any hook runs, so case 5 can
    // prove the swap really replaced them
    static unsigned char g_origGlyph[64];
    {
        const unsigned short idx = g_map[0x4E2D];
        if (idx) memcpy(g_origGlyph, (const void*)(g_bitmaps + (size_t)(idx - 1) * g_symbolSize), g_symbolSize);
    }

    // ---- 1. a mapped character, mapped by game.fnt -------------------------
    printf("\n1) draw mode, character with a game.fnt glyph (U+4E2D)\n");
    {
        DWORD before = (DWORD)(UINT_PTR)g_bitFont;
        (void)before;
        BlitCall call;
        REGISTERS r;
        DWORD espBefore = 0;
        const DWORD ret = DriveBlit(blit, 0x4E2D, 8, 4, -1, &call, &r, &espBefore);
        const int adv = GameAdvance(0x4E2D);

        if (!strcmp(mode, "draw"))
        {
            CHECK(ret == call.ret, "returns the caller's return address (0x%08X)", (unsigned)ret);
            CHECK(r.esp == espBefore + 4 + 0x10, "ESP popped return address + 4 args (0x%08X -> 0x%08X)",
                  (unsigned)espBefore, (unsigned)r.esp);
            CHECK((int)r.eax == 8 + adv, "EAX = new pen X (%d + %d = %u)", 8, adv, (unsigned)r.eax);
            CHECK(InkCount() > 20, "pixels written into the locked surface (%d)", InkCount());
        }
        else
        {
            CHECK(ret == 0, "observe mode: returns 0 (engine draws, pass-through)");
            CHECK(r.esp == espBefore, "ESP untouched (0x%08X)", (unsigned)r.esp);
            CHECK(InkCount() == 0, "nothing written (%d)", InkCount());
        }
    }

    // ---- 2. a character with no game.fnt glyph -----------------------------
    printf("\n2) character with NO game.fnt glyph (U+0400, Cyrillic)\n");
    {
        for (int i = 0; i < W * H; ++i) g_surf[i] = BG;
        InitObjects();
        if (g_map[0x0400] == 0)
        {
            BlitCall call;
            REGISTERS r;
            DWORD espBefore = 0;
            const DWORD ret = DriveBlit(blit, 0x0400, 8, 4, -1, &call, &r, &espBefore);
            CHECK(ret == 0 && r.esp == espBefore, "refused: returns 0, ESP untouched (fallback)");
            CHECK(InkCount() == 0, "nothing written (%d)", InkCount());
        }
        else
        {
            printf("  [skip] game.fnt maps U+0400 in this install\n");
        }
    }

    // ---- 3. unlocked surface ----------------------------------------------
    printf("\n3) unlocked surface (BitFont+0x0C == 0)\n");
    {
        for (int i = 0; i < W * H; ++i) g_surf[i] = BG;
        InitObjects();
        *(void**)(g_bitFont + 0x0C) = NULL;
        BlitCall call;
        REGISTERS r;
        DWORD espBefore = 0;
        const DWORD ret = DriveBlit(blit, 0x4E2D, 8, 4, -1, &call, &r, &espBefore);
        CHECK(ret == 0 && r.esp == espBefore, "refused: returns 0, ESP untouched (fallback)");

        InitObjects();       // restore for the summary below
    }

    // ---- 4. explicit colour argument --------------------------------------
    printf("\n4) explicit colour argument (arg4 != -1)\n");
    {
        for (int i = 0; i < W * H; ++i) g_surf[i] = BG;
        InitObjects();
        const unsigned short green = 0x07E0;
        BlitCall call;
        REGISTERS r;
        DriveBlit(blit, (unsigned)'A', 8, 4, green, &call, &r, NULL);
        int greenPix = 0;
        for (int i = 0; i < W * H; ++i)
            if (g_surf[i] != BG && g_surf[i] != FG) ++greenPix;
        if (!strcmp(mode, "draw"))
            CHECK(greenPix > 0, "the argument colour reached the surface (%d px)", greenPix);
        else
            CHECK(InkCount() == 0, "observe mode wrote nothing (%d)", InkCount());
    }

    // ---- 5. swap mode: our cell must replace the engine's own glyph data ---
    printf("\n5) swap mode: the engine's font data now holds our glyph\n");
    {
        const unsigned int ch = 0x4E2D;
        const unsigned short idx = g_map[ch];
        if (idx == 0)
        {
            printf("  [skip] game.fnt has no glyph for U+%04X\n", ch);
        }
        else
        {
            unsigned char* slot = (unsigned char*)(g_bitmaps + (size_t)(idx - 1) * g_symbolSize);
            static unsigned char before[64];
            memcpy(before, slot, g_symbolSize);

            BlitCall call;
            REGISTERS r;
            DWORD espBefore = 0;
            const DWORD ret = DriveBlit(blit, ch, 8, 4, -1, &call, &r, &espBefore);

            if (!strcmp(mode, "swap"))
            {
                CHECK(ret == 0 && r.esp == espBefore, "no skip at all: returns 0, ESP untouched");
                CHECK(memcmp(g_origGlyph, slot, g_symbolSize) != 0,
                      "the font's glyph data differs from the original game.fnt bytes");
                CHECK(slot[0] == before[0], "advance byte unchanged (%u)", (unsigned)slot[0]);

                static unsigned char after[64];
                memcpy(after, slot, g_symbolSize);
                DriveBlit(blit, ch, 8, 4, -1, &call, &r, NULL);
                CHECK(memcmp(after, slot, g_symbolSize) == 0, "second call is idempotent");

                // the engine will read these bytes: they must be a valid 1bpp cell
                int ink = 0;
                for (int i = 1; i < g_symbolSize; ++i)
                    for (int b = 0; b < 8; ++b)
                        if (slot[i] & (0x80 >> b)) ++ink;
                CHECK(ink > 10, "the swapped cell has ink (%d pixels)", ink);
            }
            else
            {
                CHECK(memcmp(before, slot, g_symbolSize) == 0,
                      "mode=%s leaves the font data alone", mode);
            }
        }
    }

    printf("\n[*] failures=%d\n", g_fails);
    FreeLibrary(dll);        // DLL_PROCESS_DETACH -> final summary in VectorText.log
    return g_fails ? 1 : 0;
}
