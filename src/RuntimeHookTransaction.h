// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <array>
#include <cstring>

namespace vt::RuntimeHooks::detail
{
enum class CommitResult { Committed, RolledBack, UnsafeRollback };
template <std::size_t Span> struct Patch
{
    unsigned char *address = nullptr;
    std::size_t size = 0;
    std::array<unsigned char, Span> original{}, replacement{};
};
struct Page
{
    void *address = nullptr;
    DWORD protection = 0;
    bool writable = false;
    bool changed = false;
};

// No allocations, logging, locks or callbacks into the game while peers are
// suspended. Ops only changes protection, flushes caches and records errors.
template <std::size_t Count, std::size_t Span, class Ops>
[[nodiscard]] CommitResult Commit(const std::array<Patch<Span>, Count> &patches,
                                  std::array<Page, Count * 2> &pages, std::size_t pageCount, Ops &ops) noexcept
{
    static_assert(noexcept(ops.Protect(nullptr, DWORD{}, pages[0].protection, "")),
                  "Protection operations must not throw during a transaction.");
    static_assert(noexcept(ops.Flush(nullptr, std::size_t{}, "")) && noexcept(ops.Mismatch(nullptr)),
                  "Cache operations and diagnostics must not throw during a transaction.");
    bool written = false;
    const auto restorePages = [&] {
        bool ok = true;
        for (std::size_t i = pageCount; i-- > 0;)
        {
            auto &page = pages[i];
            if (!page.changed || !page.writable)
                continue;
            DWORD ignored = 0;
            if (ops.Protect(page.address, page.protection, ignored, "restore protection"))
                page.writable = false;
            else
                ok = false;
        }
        return ok;
    };
    const auto rollback = [&] {
        bool ok = true;
        if (written)
        {
            // A failed commit may already have restored some pages to RX.
            for (std::size_t i = 0; i < pageCount; ++i)
            {
                auto &page = pages[i];
                if (page.writable)
                    continue;
                DWORD ignored = 0;
                if (ops.Protect(page.address, PAGE_EXECUTE_READWRITE, ignored, "rollback writable"))
                    page.writable = true;
                else
                    ok = false;
            }
            // Never attempt a store into a page we failed to make writable.
            if (ok)
                for (const auto &patch : patches)
                {
                    std::memcpy(patch.address, patch.original.data(), patch.size);
                    if (!ops.Flush(patch.address, patch.size, "rollback cache"))
                        ok = false;
                }
        }
        if (!restorePages())
            ok = false;
        return ok ? CommitResult::RolledBack : CommitResult::UnsafeRollback;
    };
    for (const auto &patch : patches)
        if (std::memcmp(patch.address, patch.original.data(), patch.size))
        {
            ops.Mismatch(patch.address);
            return CommitResult::RolledBack;
        }
    for (std::size_t i = 0; i < pageCount; ++i)
    {
        auto &page = pages[i];
        if (!ops.Protect(page.address, PAGE_EXECUTE_READWRITE, page.protection, "commit writable"))
            return rollback();
        page.changed = page.writable = true;
    }
    // Final all-sites validation covers the permission acquisition window.
    for (const auto &patch : patches)
        if (std::memcmp(patch.address, patch.original.data(), patch.size))
        {
            ops.Mismatch(patch.address);
            return rollback();
        }
    written = true;
    for (const auto &patch : patches)
        std::memcpy(patch.address, patch.replacement.data(), patch.size);
    for (const auto &patch : patches)
        if (!ops.Flush(patch.address, patch.size, "commit cache"))
            return rollback();
    if (!restorePages())
        return rollback();
    return CommitResult::Committed;
}
} // namespace vt::RuntimeHooks::detail
