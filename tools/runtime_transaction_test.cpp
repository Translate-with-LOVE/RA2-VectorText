// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "RuntimeHooks.h"
#include "RuntimeHookPlatform.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>

namespace
{
using namespace vt::RuntimeHooks;
using namespace vt::RuntimeHooks::detail;
int failures = 0;
void Check(bool ok, const char *message)
{
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
DWORD __cdecl Handler(REGISTERS *) { return 0; }
struct Fixture
{
    std::array<std::array<unsigned char, 8>, 3> bytes{};
    std::array<Patch<8>, 3> patches{};
    std::array<Page, 6> pages{};
    std::array<DWORD, 2> protection{PAGE_EXECUTE_READ, PAGE_READONLY};
    std::array<DWORD, 2> initial = protection;
    int protectCalls = 0, flushCalls = 0, errors = 0;
    int failProtect = 0, failFlush = 0, failAgain = 0, failFlushAgain = 0;
    bool mutateOnProtect = false;
    Fixture()
    {
        for (size_t i = 0; i < patches.size(); ++i)
        {
            bytes[i].fill(static_cast<unsigned char>(0x40 + i));
            patches[i] = {bytes[i].data(), bytes[i].size(), bytes[i], {}};
            patches[i].replacement.fill(static_cast<unsigned char>(0xE0 + i));
        }
        // First two sites share one page; the third spans both pages.
        pages[0].address = &protection[0]; pages[1].address = &protection[1];
    }
    bool Protect(void *address, DWORD desired, DWORD &old, const char *) noexcept
    {
        ++protectCalls;
        if (protectCalls == failProtect || protectCalls == failAgain) { ++errors; return false; }
        auto &value = *static_cast<DWORD *>(address);
        old = value; value = desired;
        if (mutateOnProtect) { bytes[2][0] ^= 1; mutateOnProtect = false; }
        return true;
    }
    bool Flush(void *, size_t, const char *) noexcept
    {
        ++flushCalls;
        if (flushCalls == failFlush || flushCalls == failFlushAgain) { ++errors; return false; }
        return true;
    }
    void Mismatch(void *) noexcept { ++errors; }
    CommitResult Run() { return Commit(patches, pages, 2, *this); }
    bool Original() const
    {
        for (size_t i = 0; i < patches.size(); ++i) if (bytes[i] != patches[i].original) return false;
        return true;
    }
};
void Transactions()
{
    Fixture success;
    Check(success.Run() == CommitResult::Committed && success.protection == success.initial, "commit restores distinct page protections");
    for (size_t i = 0; i < success.patches.size(); ++i)
        Check(success.bytes[i] == success.patches[i].replacement, "commit installs every site");
    for (int failure = 1; failure <= 4; ++failure)
    {
        Fixture f; f.failProtect = failure;
        Check(f.Run() == CommitResult::RolledBack && f.Original() && f.protection == f.initial && f.errors == 1,
              "any acquisition or restoration failure rolls back all bytes and page protections");
    }
    for (int failure = 1; failure <= 3; ++failure)
    {
        Fixture f; f.failFlush = failure;
        Check(f.Run() == CommitResult::RolledBack && f.Original() && f.protection == f.initial,
              "cache failure rolls back all sites");
    }
    Fixture mismatch; mismatch.bytes[1][0] ^= 1;
    const auto before = mismatch.bytes;
    Check(mismatch.Run() == CommitResult::RolledBack && mismatch.bytes == before && mismatch.protectCalls == 0,
          "late validation rejects mismatches without touching pages");
    Fixture race; race.mutateOnProtect = true;
    Check(race.Run() == CommitResult::RolledBack && race.bytes[2][0] == (race.patches[2].original[0] ^ 1) &&
          race.bytes[0] == race.patches[0].original && race.protection == race.initial,
          "final validation preserves a third-party change during protection acquisition");
    Fixture unsafe; unsafe.failProtect = 3; unsafe.failAgain = 5;
    Check(unsafe.Run() == CommitResult::UnsafeRollback, "failed rollback write access is never reported as success");
    Fixture unsafeRestore; unsafeRestore.failProtect = 3; unsafeRestore.failAgain = 6;
    Check(unsafeRestore.Run() == CommitResult::UnsafeRollback && unsafeRestore.Original(),
          "failed rollback protection is reported even after bytes are restored");
    Fixture unsafeCache; unsafeCache.failFlush = 2; unsafeCache.failProtect = 3;
    Check(unsafeCache.Run() == CommitResult::UnsafeRollback && unsafeCache.Original(), "rollback cannot hide protection failure");
    Fixture unsafeFlush; unsafeFlush.failFlush = 1; unsafeFlush.failFlushAgain = 2;
    Check(unsafeFlush.Run() == CommitResult::UnsafeRollback && unsafeFlush.Original() && unsafeFlush.protection == unsafeFlush.initial,
          "rollback cache failure is reported even after restoring bytes and permissions");
}
void NativePages()
{
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const auto pageSize = system.dwPageSize;
    for (int failure = 0; failure <= 4; ++failure)
    {
        auto *memory = static_cast<unsigned char *>(VirtualAlloc(nullptr, pageSize * 2,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Check(memory != nullptr, "native page fixture allocated");
        if (!memory) return;
        std::array<Patch<8>, 2> patches{};
        patches[0].address = memory + pageSize - 4; // deliberately straddles two pages
        patches[1].address = memory + pageSize + 40; // shares the second page
        for (auto &patch : patches)
        {
            patch.size = 8; patch.original.fill(0x45); patch.replacement.fill(0x90);
            std::memcpy(patch.address, patch.original.data(), patch.size);
        }
        DWORD old = 0;
        Check(VirtualProtect(memory, pageSize, PAGE_EXECUTE_READ, &old) &&
              VirtualProtect(memory + pageSize, pageSize, PAGE_READONLY, &old), "native pages have distinct original protection");
        std::array<Page, 4> pages{};
        pages[0].address = memory; pages[1].address = memory + pageSize;
        Diagnostics errors;
        struct InjectedOps : WindowsOps
        {
            int call = 0, fail = 0;
            bool Protect(void *at, DWORD desired, DWORD &previous, const char *phase) noexcept
            {
                if (++call == fail) { diagnostics.Record(phase, at, ERROR_ACCESS_DENIED); return false; }
                return WindowsOps::Protect(at, desired, previous, phase);
            }
        } ops{{pageSize, errors}, 0, failure};
        const auto result = Commit(patches, pages, 2, ops);
        Check(result == (failure ? CommitResult::RolledBack : CommitResult::Committed), "native page commit and rollback result");
        for (const auto &patch : patches)
            Check(!std::memcmp(patch.address, failure ? patch.original.data() : patch.replacement.data(), patch.size),
                  "shared and cross-page native sites are fully committed or restored");
        MEMORY_BASIC_INFORMATION first{}, second{};
        Check(VirtualQuery(memory, &first, sizeof(first)) && VirtualQuery(memory + pageSize, &second, sizeof(second)) &&
              first.Protect == PAGE_EXECUTE_READ && second.Protect == PAGE_READONLY, "native page protections restored exactly");
        VirtualFree(memory, 0, MEM_RELEASE);
    }
}
void Stubs()
{
    constexpr std::array<unsigned char, 5> original{0x83, 0xEC, 0x30, 0x53, 0x55};
    std::array<unsigned char, StubSize(original.size()) + 2> buffer;
    buffer.fill(0xAA);
    const auto before = buffer;
    Check(!BuildStub({buffer.data() + 1, StubSize(original.size()) - 1}, 0x401000, original.size(), original.data(), Handler) &&
          buffer == before, "undersized stub buffer fails without writing");
    Check(!BuildStub({buffer.data(), buffer.size()}, 0, std::numeric_limits<unsigned int>::max(), original.data(), Handler) &&
          buffer == before, "oversized span fails without arithmetic overflow");
    Check(!BuildStub({nullptr, buffer.size()}, 0, 5, original.data(), Handler), "null stub buffer rejected");
    Check(!BuildStub({buffer.data(), buffer.size()}, 0, 5, nullptr, Handler) && buffer == before, "null replay bytes rejected");
    Check(!BuildStub({buffer.data(), buffer.size()}, 0, 5, original.data(), nullptr) && buffer == before, "null handler rejected");
    const auto size = BuildStub({buffer.data() + 1, StubSize(original.size())}, 0x401000, original.size(), original.data(), Handler);
    Check(size && *size == StubSize(original.size()) && buffer.front() == 0xAA && buffer.back() == 0xAA,
          "exact capacity succeeds and preserves canaries");
    Check(BuildStub({buffer.data(), buffer.size()}, 0, 0, nullptr, Handler) == StubSize(0), "zero replay test stub remains supported");
}
DWORD WINAPI WaitWorker(void *event) { WaitForSingleObject(event, INFINITE); return 0; }
void Threads()
{
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    UniqueHandle worker(CreateThread(nullptr, 0, WaitWorker, event.get(), CREATE_SUSPENDED, nullptr));
    Check(event && worker, "suspended peer fixture created");
    if (!event || !worker) return;
    Diagnostics errors;
    Fixture f;
    {
        StartupThreads guard(errors);
        Check(guard.Prepare() && guard.Acquire(f.patches), "startup guard holds peers with preexisting suspension");
        guard.Release();
    }
    Check(ResumeThread(worker.get()) == 1, "guard preserves the injector's original suspend count");
    SetEvent(event.get());
    WaitForSingleObject(worker.get(), 5000);
    ResetEvent(event.get());
    worker.reset(CreateThread(nullptr, 0, WaitWorker, event.get(), CREATE_SUSPENDED, nullptr));
    CONTEXT originalContext{}; originalContext.ContextFlags = CONTEXT_CONTROL;
    Check(worker && GetThreadContext(worker.get(), &originalContext), "peer context fixture captured");
    CONTEXT inside = originalContext; inside.Eip = reinterpret_cast<DWORD>(f.patches[0].address) + 1;
    Check(SetThreadContext(worker.get(), &inside) != FALSE, "peer EIP placed inside overwrite span");
    {
        Diagnostics insideErrors;
        StartupThreads guard(insideErrors);
        const bool prepared = guard.Prepare();
        const bool acquired = prepared && guard.Acquire(f.patches);
        guard.Release();
        Check(prepared && !acquired && insideErrors.error == ERROR_BUSY, "peer inside overwrite span aborts installation");
    }
    Check(SetThreadContext(worker.get(), &originalContext) != FALSE, "peer EIP restored after refusal");
    ResumeThread(worker.get()); SetEvent(event.get()); WaitForSingleObject(worker.get(), 5000);
    // A new peer between preparation and acquisition must cause rejection.
    ResetEvent(event.get());
    {
        StartupThreads guard(errors);
        Check(guard.Prepare(), "late peer fixture prepared");
        worker.reset(CreateThread(nullptr, 0, WaitWorker, event.get(), CREATE_SUSPENDED, nullptr));
        Check(worker && !guard.Acquire(f.patches), "new peer aborts before any code write");
    }
    if (worker) { ResumeThread(worker.get()); SetEvent(event.get()); WaitForSingleObject(worker.get(), 5000); }
    // A runnable peer receives exactly one suspension, then returns to runnable.
    ResetEvent(event.get());
    worker.reset(CreateThread(nullptr, 0, WaitWorker, event.get(), 0, nullptr));
    Diagnostics runnableErrors;
    bool prepared = false, acquired = false;
    DWORD heldCount = DWORD(-1);
    {
        StartupThreads guard(runnableErrors);
        prepared = worker && guard.Prepare();
        acquired = prepared && guard.Acquire(f.patches);
        if (acquired)
        {
            heldCount = SuspendThread(worker.get());
            if (heldCount != DWORD(-1)) ResumeThread(worker.get());
        }
    }
    Check(prepared && acquired && heldCount == 1, "runnable peer is held suspended through the transaction interval");
    if (worker)
    {
        const auto count = SuspendThread(worker.get());
        if (count != DWORD(-1)) ResumeThread(worker.get());
        Check(count == 0, "guard destruction restores runnable peer suspension count");
        SetEvent(event.get()); WaitForSingleObject(worker.get(), 5000);
    }
}
} // namespace
int main()
{
    Transactions(); NativePages(); Stubs(); Threads();
    std::printf("runtime transaction: %d failures\n", failures);
    return failures ? 1 : 0;
}
