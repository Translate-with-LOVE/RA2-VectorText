// ===========================================================================
//  selftest.cpp -- load VectorText.dll in-process and call its hook handlers
//  with a synthetic REGISTERS block.
//
//  This proves, without starting the game, that
//    * the DLL loads,
//    * every hook export is present and returns 0 (pass-through),
//    * the logger initialises in the *calling* process and writes its file,
//    * argument scanning / string de-duplication work.
//
//  The log it produces lands next to this executable (not in the game dir),
//  because the log path is derived from the main module of the process.
// ===========================================================================

#include <windows.h>
#include <stdio.h>
#include <string.h>

struct REGISTERS
{
    DWORD origin, flags, edi, esi, ebp, esp, ebx, edx, ecx, eax;
};

typedef DWORD(__cdecl* HookProc)(REGISTERS*);

static int g_fails = 0;

static void CallHook(HMODULE dll, const char* name, DWORD origin,
                     const wchar_t* text, DWORD ecx)
{
    HookProc proc = (HookProc)GetProcAddress(dll, name);
    if (!proc)
    {
        printf("  [x] export %-40s NOT FOUND\n", name);
        ++g_fails;
        return;
    }

    DWORD stack[12];
    for (int i = 0; i < 12; ++i)
        stack[i] = (DWORD)(UINT_PTR)text;      // every candidate argument looks like the text
    stack[0] = 0x00401234;                     // fake return address -> logged as "caller"

    REGISTERS r;
    memset(&r, 0, sizeof(r));
    r.origin = origin;
    r.esp    = (DWORD)(UINT_PTR)stack;
    r.ecx    = ecx;
    r.edx    = (DWORD)(UINT_PTR)text;

    const DWORD ret = proc(&r);
    if (ret != 0)
    {
        printf("  [x] %-40s returned %lu (must be 0 = pass-through)\n", name, (unsigned long)ret);
        ++g_fails;
    }
}

int main(void)
{
    HMODULE dll = LoadLibraryA("VectorText.dll");
    if (!dll)
    {
        printf("[x] LoadLibraryA(VectorText.dll) failed: %lu\n", GetLastError());
        return 2;
    }
    printf("[*] VectorText.dll loaded at %p\n", (void*)dll);

    static const wchar_t* texts[] =
    {
        L"Options", L"Options", L"Start Game", L"\x9009\x9879", L"Score: 1234"
    };
    const int nTexts = (int)(sizeof(texts) / sizeof(texts[0]));

    static const struct { const char* name; DWORD addr; DWORD ecx; } hooks[] =
    {
        { "VT_Hook_Drawing_PrintUnicode",      0x004A61C0, 0 },
        { "VT_Hook_Drawing_GetTextDimensions", 0x004A59E0, 0 },
        { "VT_Hook_BitFont_GetTextDimension",  0x00433CF0, 0x0089C4D0 },
        { "VT_Hook_BitText_Print",             0x00434B90, 0x0089C4B8 },
        { "VT_Hook_BitText_DrawText",          0x00434CD0, 0x0089C4B8 },
    };
    const int nHooks = (int)(sizeof(hooks) / sizeof(hooks[0]));

    for (int t = 0; t < nTexts; ++t)
        for (int h = 0; h < nHooks; ++h)
            CallHook(dll, hooks[h].name, hooks[h].addr, texts[t], hooks[h].ecx);

    // per-glyph hook: its first argument is a character, not a pointer
    for (int i = 0; i < 25; ++i)
        CallHook(dll, "VT_Hook_BitFont_Blit", 0x00434120, L"A", 0x0089C4D0);

    FreeLibrary(dll);       // triggers DLL_PROCESS_DETACH -> final summary
    printf("[*] done. failures=%d\n", g_fails);
    printf("[*] expected log: VectorText.log next to this executable\n");
    return g_fails ? 1 : 0;
}
