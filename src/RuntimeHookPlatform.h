// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "RuntimeHookTransaction.h"
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace vt::RuntimeHooks::detail
{
struct Diagnostics
{
    const char *phase = nullptr;
    const void *address = nullptr;
    DWORD error = 0;
    const char *lastPhase = nullptr;
    const void *lastAddress = nullptr;
    DWORD lastError = 0;
    unsigned int failures = 0;
    void Record(const char *where, const void *at, DWORD code) noexcept
    {
        // Preserve the triggering failure; count subsequent rollback failures.
        if (!phase) { phase = where; address = at; error = code; }
        lastPhase = where; lastAddress = at; lastError = code;
        ++failures;
    }
    void Report() const noexcept
    {
        if (!phase) return;
        char message[256]{};
        std::snprintf(message, sizeof(message),
            "VectorText: hook installation failed: %s at %p, Win32 error=%lu, failures=%u\n",
            phase, address, error, failures);
        OutputDebugStringA(message); // regular logger is not initialized in DllMain
        if (failures > 1)
        {
            std::snprintf(message, sizeof(message), "VectorText: last hook failure: %s at %p, Win32 error=%lu\n",
                lastPhase, lastAddress, lastError);
            OutputDebugStringA(message);
        }
    }
};
[[noreturn]] inline void FailClosed(const Diagnostics &errors) noexcept
{
    // Returning FALSE would unload handlers still reachable from patched code.
    // Continuing after an incomplete rollback or lost suspension is unsafe.
    // Preserve diagnostic details in the fail-fast exception without formatting
    // text or entering logger/CRT locks while other threads remain suspended.
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xC0000602u; // STATUS_FAIL_FAST_EXCEPTION (Win7)
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = reinterpret_cast<void *>(&FailClosed);
    record.NumberParameters = 6;
    record.ExceptionInformation[0] = errors.error;
    record.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(errors.address);
    record.ExceptionInformation[2] = reinterpret_cast<ULONG_PTR>(errors.phase);
    record.ExceptionInformation[3] = errors.lastError;
    record.ExceptionInformation[4] = reinterpret_cast<ULONG_PTR>(errors.lastAddress);
    record.ExceptionInformation[5] = reinterpret_cast<ULONG_PTR>(errors.lastPhase);
    RaiseFailFastException(&record, nullptr, 0);
    std::abort();
}
struct CloseHandleDeleter
{
    void operator()(void *handle) const noexcept { CloseHandle(handle); }
};
using UniqueHandle = std::unique_ptr<void, CloseHandleDeleter>;
inline UniqueHandle ThreadSnapshot()
{
    auto handle = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    return UniqueHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}
class StartupThreads
{
    struct Thread { DWORD id; UniqueHandle handle; bool suspended = false; };
    std::vector<Thread> threads;
    Diagnostics &diagnostics;
    bool released = false;
public:
    explicit StartupThreads(Diagnostics &errors) : diagnostics(errors) {}
    StartupThreads(const StartupThreads &) = delete;
    StartupThreads &operator=(const StartupThreads &) = delete;
    ~StartupThreads() { Release(); }
    // Prepare handles and storage before suspending anybody. DllMain's loader
    // lock prevents new threads entering user code during this startup window.
    [[nodiscard]] bool Prepare()
    {
        auto snapshot = ThreadSnapshot();
        if (!snapshot) { diagnostics.Record("thread snapshot", nullptr, GetLastError()); return false; }
        THREADENTRY32 entry{sizeof(entry)};
        if (!Thread32First(snapshot.get(), &entry))
        { diagnostics.Record("thread enumeration", nullptr, GetLastError()); return false; }
        do
        {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId())
                continue;
            UniqueHandle handle(OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID));
            if (!handle) { diagnostics.Record("open thread", nullptr, GetLastError()); return false; }
            threads.push_back({entry.th32ThreadID, std::move(handle)});
        } while (Thread32Next(snapshot.get(), &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES)
        { diagnostics.Record("thread enumeration", nullptr, GetLastError()); return false; }
        return true;
    }
    template <std::size_t Count, std::size_t Span>
    [[nodiscard]] bool Acquire(const std::array<Patch<Span>, Count> &patches) noexcept
    {
        for (auto &thread : threads)
        {
            if (SuspendThread(thread.handle.get()) == DWORD(-1))
            { diagnostics.Record("suspend thread", nullptr, GetLastError()); return false; }
            thread.suspended = true; // exactly one extra suspend count owned by us
        }
        for (const auto &thread : threads)
        {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread.handle.get(), &context))
            { diagnostics.Record("thread context", nullptr, GetLastError()); return false; }
            for (const auto &patch : patches)
            {
                const auto start = reinterpret_cast<DWORD>(patch.address);
                if (context.Eip >= start && context.Eip - start < patch.size)
                { diagnostics.Record("thread inside hook span", patch.address, ERROR_BUSY); return false; }
            }
        }
        // Catch a thread created between Prepare and Acquire. Abort instead of
        // allocating more handles/storage while other threads are suspended.
        auto snapshot = ThreadSnapshot();
        if (!snapshot) { diagnostics.Record("verify thread snapshot", nullptr, GetLastError()); return false; }
        THREADENTRY32 entry{sizeof(entry)};
        if (!Thread32First(snapshot.get(), &entry))
        { diagnostics.Record("verify threads", nullptr, GetLastError()); return false; }
        do
        {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId())
                continue;
            bool held = false;
            for (const auto &thread : threads) held = held || thread.id == entry.th32ThreadID;
            if (!held) { diagnostics.Record("new thread during preparation", nullptr, ERROR_BUSY); return false; }
        } while (Thread32Next(snapshot.get(), &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES)
        { diagnostics.Record("verify threads", nullptr, GetLastError()); return false; }
        return true;
    }
    void Release() noexcept
    {
        if (released) return;
        for (auto &thread : threads)
            if (thread.suspended)
            {
                if (ResumeThread(thread.handle.get()) == DWORD(-1))
                {
                    diagnostics.Record("resume thread", nullptr, GetLastError());
                    FailClosed(diagnostics);
                }
                thread.suspended = false;
            }
        released = true;
    }
};
struct WindowsOps
{
    std::size_t pageSize;
    Diagnostics &diagnostics;
    bool Protect(void *address, DWORD protection, DWORD &old, const char *phase) noexcept
    {
        if (VirtualProtect(address, pageSize, protection, &old)) return true;
        diagnostics.Record(phase, address, GetLastError());
        return false;
    }
    bool Flush(void *address, std::size_t size, const char *phase) noexcept
    {
        if (FlushInstructionCache(GetCurrentProcess(), address, size)) return true;
        diagnostics.Record(phase, address, GetLastError());
        return false;
    }
    void Mismatch(void *address) noexcept { diagnostics.Record("hook byte mismatch", address, ERROR_INVALID_DATA); }
};
} // namespace vt::RuntimeHooks::detail
