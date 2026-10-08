// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "RuntimeHooks.h"
#include "GameAddresses.h"
#include <cstring>

#define VT_RUNTIME_HOOK(field, handler, span, yrBytes, raBytes) extern "C" DWORD __cdecl handler(REGISTERS *);
#include "RuntimeHookSites.inc"
#undef VT_RUNTIME_HOOK

namespace vt::RuntimeHooks
{
namespace
{
void Jump(unsigned char *at, const void *target)
{
    at[0] = 0xE9;
    *(DWORD *)(at + 1) = (DWORD)target - (DWORD)at - 5;
}
} // namespace

bool Install(bool ra2)
{
    struct Site
    {
        DWORD address, span;
        DWORD(__cdecl *handler)(REGISTERS *);
        const char *expected;
    };
#define VT_RUNTIME_HOOK(field, handler, span, yrBytes, raBytes)                                                        \
    {game::field, game::span, handler, ra2 ? raBytes : yrBytes},
    const Site sites[] = {
#include "RuntimeHookSites.inc"
    };
#undef VT_RUNTIME_HOOK
    constexpr unsigned int count = sizeof(sites) / sizeof(sites[0]);
    for (const Site &site : sites)
        if (memcmp((void *)site.address, site.expected, site.span))
            return false;
    auto stubs = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!stubs)
        return false;
    for (unsigned int i = 0; i < count; ++i)
        BuildStub(stubs + i * 128, sites[i].address, sites[i].span, (const unsigned char *)sites[i].expected,
                  sites[i].handler);
    DWORD old = 0;
    if (!VirtualProtect(stubs, 4096, PAGE_EXECUTE_READ, &old))
    {
        VirtualFree(stubs, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, 4096);
    struct Page
    {
        void *address;
        DWORD protection;
    };
    Page pages[count * 2] = {};
    unsigned int pageCount = 0;
    bool writable = true;
    for (const Site &site : sites)
    {
        // Supported Windows x86 uses 4 KiB pages. Deduplicate shared pages so
        // their original protection is recorded before any text is modified.
        const DWORD first = site.address & ~0xFFFu;
        const DWORD last = (site.address + site.span - 1) & ~0xFFFu;
        for (DWORD page = first; page <= last; page += 4096)
        {
            bool found = false;
            for (unsigned int j = 0; j < pageCount; ++j)
                found = found || pages[j].address == (void *)page;
            if (found)
                continue;
            if (!VirtualProtect((void *)page, 4096, PAGE_EXECUTE_READWRITE, &old))
            {
                writable = false;
                break;
            }
            pages[pageCount++] = {(void *)page, old};
        }
        if (!writable)
            break;
    }
    if (writable)
    {
        for (unsigned int i = 0; i < count; ++i)
        {
            const Site &site = sites[i];
            auto address = (unsigned char *)site.address;
            Jump(address, stubs + i * 128);
            memset(address + 5, 0x90, site.span - 5);
            FlushInstructionCache(GetCurrentProcess(), (void *)site.address, site.span);
        }
    }
    for (unsigned int i = 0; i < pageCount; ++i)
    {
        DWORD ignored;
        VirtualProtect(pages[i].address, 4096, pages[i].protection, &ignored);
    }
    if (!writable)
        VirtualFree(stubs, 0, MEM_RELEASE);
    return writable; // successful stubs live for the game process lifetime
}
} // namespace vt::RuntimeHooks

// A common five-byte prologue, verified in both executables, lets unmodified
// Syringe discover/load this DLL. All text hooks use the selected runtime table.
VT_HOOK_DECL(0x401000, VT_Bootstrap, 5)
VT_HOOK_FUNC(VT_Bootstrap)
{
    (void)R;
    return 0;
}
