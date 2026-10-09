// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "RuntimeHooks.h"
#include "RuntimeHookPlatform.h"
#include "GameAddresses.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>

#define VT_RUNTIME_HOOK(field, handler, span, yrBytes, raBytes) extern "C" DWORD __cdecl handler(REGISTERS *);
#include "RuntimeHookSites.inc"
#undef VT_RUNTIME_HOOK

namespace vt::RuntimeHooks
{
namespace
{
struct Site
{
    DWORD yrAddress, raAddress;
    unsigned int span;
    Handler handler;
    const char *yrBytes, *raBytes;
};
#define VT_RUNTIME_HOOK(field, handler, span, yrBytes, raBytes) \
    static_assert(yra::span == ra2a::span && yra::span >= code::jumpSize); \
    static_assert(sizeof(yrBytes) - 1 == yra::span && sizeof(raBytes) - 1 == ra2a::span);
#include "RuntimeHookSites.inc"
#undef VT_RUNTIME_HOOK
#define VT_RUNTIME_HOOK(field, handler, span, yrBytes, raBytes) \
    Site{yra::field, ra2a::field, yra::span, handler, yrBytes, raBytes},
constexpr std::array sites{
#include "RuntimeHookSites.inc"
};
#undef VT_RUNTIME_HOOK
static_assert([] {
    for (bool ra2 : {false, true})
        for (std::size_t i = 0; i < sites.size(); ++i)
        {
            const auto address = ra2 ? sites[i].raAddress : sites[i].yrAddress;
            if (address > std::numeric_limits<DWORD>::max() - sites[i].span) return false;
            for (std::size_t j = 0; j < i; ++j)
            {
                const auto other = ra2 ? sites[j].raAddress : sites[j].yrAddress;
                if (address < other + sites[j].span && other < address + sites[i].span) return false;
            }
        }
    return true;
}(), "Hook ranges must not overflow or overlap in either executable.");
constexpr std::size_t maxSpan = [] {
    std::size_t result = 0;
    for (const auto &site : sites) result = std::max(result, static_cast<std::size_t>(site.span));
    return result;
}();
constexpr auto stubStride = StubSize(maxSpan);
static_assert(sites.size() <= std::numeric_limits<std::size_t>::max() / stubStride);
constexpr auto stubAllocationSize = sites.size() * stubStride;
using Patches = std::array<detail::Patch<maxSpan>, sites.size()>;
using Pages = std::array<detail::Page, sites.size() * 2>;
struct FreeMemory
{
    void operator()(unsigned char *memory) const noexcept { VirtualFree(memory, 0, MEM_RELEASE); }
};
using StubMemory = std::unique_ptr<unsigned char, FreeMemory>;

bool InstallPrepared(bool ra2, detail::Diagnostics &errors)
{
    // prepare: all allocation and machine-code generation precedes suspension.
    Patches patches{};
    for (std::size_t i = 0; i < sites.size(); ++i)
    {
        const auto &site = sites[i];
        auto &patch = patches[i];
        patch.address = reinterpret_cast<unsigned char *>(ra2 ? site.raAddress : site.yrAddress);
        patch.size = site.span;
        std::memcpy(patch.original.data(), ra2 ? site.raBytes : site.yrBytes, patch.size);
        if (std::memcmp(patch.address, patch.original.data(), patch.size))
        { errors.Record("prepare byte mismatch", patch.address, ERROR_INVALID_DATA); return false; }
    }
    StubMemory stubs(static_cast<unsigned char *>(VirtualAlloc(nullptr, stubAllocationSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)));
    if (!stubs) { errors.Record("allocate stubs", nullptr, GetLastError()); return false; }
    for (std::size_t i = 0; i < sites.size(); ++i)
    {
        auto &patch = patches[i];
        auto *stub = stubs.get() + i * stubStride;
        const auto size = BuildStub({stub, stubStride}, reinterpret_cast<DWORD>(patch.address),
            sites[i].span, patch.original.data(), sites[i].handler);
        if (!size) { errors.Record("build stub", stub, ERROR_INSUFFICIENT_BUFFER); return false; }
        patch.replacement.fill(0x90);
        patch.replacement[0] = 0xE9;
        const DWORD relative = reinterpret_cast<DWORD>(stub) - reinterpret_cast<DWORD>(patch.address) - code::jumpSize;
        std::memcpy(patch.replacement.data() + 1, &relative, sizeof(relative));
    }
    DWORD old = 0;
    if (!VirtualProtect(stubs.get(), stubAllocationSize, PAGE_EXECUTE_READ, &old))
    { errors.Record("protect stubs", stubs.get(), GetLastError()); return false; }
    if (!FlushInstructionCache(GetCurrentProcess(), stubs.get(), stubAllocationSize))
    { errors.Record("flush stubs", stubs.get(), GetLastError()); return false; }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::size_t pageSize = system.dwPageSize;
    // A span shorter than one page can touch at most two pages.
    if (pageSize < maxSpan) { errors.Record("page size", nullptr, ERROR_NOT_SUPPORTED); return false; }
    Pages pages{};
    std::size_t pageCount = 0;
    for (const auto &patch : patches)
    {
        const auto address = reinterpret_cast<std::uintptr_t>(patch.address);
        const auto first = address / pageSize * pageSize;
        const auto last = (address + patch.size - 1) / pageSize * pageSize;
        for (auto page = first;; page += pageSize)
        {
            auto *at = reinterpret_cast<void *>(page);
            bool found = false;
            for (std::size_t i = 0; i < pageCount; ++i) found = found || pages[i].address == at;
            if (!found) pages[pageCount++].address = at;
            if (page == last) break;
        }
    }
    detail::StartupThreads threads(errors);
    if (!threads.Prepare() || !threads.Acquire(patches)) return false;
    detail::WindowsOps ops{pageSize, errors};
    // validate -> commit -> rollback all run within the same quiescent interval.
    const auto result = detail::Commit(patches, pages, pageCount, ops);
    if (result == detail::CommitResult::UnsafeRollback)
    {
        // Keep all threads stopped. Returning would unload the DLL with live
        // hooks or let the game execute code with unconfirmed cache/protection.
        detail::FailClosed(errors);
    }
    if (result == detail::CommitResult::Committed)
        (void)stubs.release(); // committed stubs live for the game process lifetime
    threads.Release();
    return result == detail::CommitResult::Committed;
}
} // namespace

bool Install(bool ra2)
{
    detail::Diagnostics errors;
    bool installed = false;
    try { installed = InstallPrepared(ra2, errors); }
    catch (const std::bad_alloc &) { errors.Record("prepare storage", nullptr, ERROR_NOT_ENOUGH_MEMORY); }
    errors.Report();
    return installed;
}
} // namespace vt::RuntimeHooks

// A common five-byte prologue lets unmodified Syringe discover/load this DLL.
VT_HOOK_DECL(0x401000, VT_Bootstrap, 5)
VT_HOOK_FUNC(VT_Bootstrap)
{
    (void)R;
    return 0;
}
