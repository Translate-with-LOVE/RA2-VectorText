// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace runtime_stub_check
{
static bool redirect;
static DWORD resume;
static DWORD __cdecl Handler(REGISTERS *r)
{
    CHECK(r->Origin() != 0 && r->Stack32(4) == 41, "runtime stub captures original ESP and arguments");
    CHECK(r->ECX() == 0x11223344 && r->EDX() == 0x55667788, "runtime stub captures input registers");
    r->EAX(777);
    r->ECX(123);
    r->EDX(456);
    r->EFLAGS(r->EFLAGS() | 1);
    return redirect ? resume : 0;
}
static void Check()
{
    auto code = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    CHECK(code != NULL, "runtime stub fixture allocated");
    if (!code)
        return;
    const unsigned char original[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xC0, 0x01, 0xC2, 0x04, 0x00};
    memcpy(code, original, sizeof(original));
    resume = (DWORD)(code + 7);
    const unsigned int bytes = vt::RuntimeHooks::BuildStub(code + 128, (DWORD)code, 7, original, Handler);
    CHECK(bytes <= 128, "runtime stub stays within its reserved slot");
    dimension_machine::Jump(code, code + 128);
    code[5] = code[6] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    for (int mode = 0; mode < 2; ++mode)
    {
        redirect = mode != 0;
        DWORD result, ecxResult, edxResult, flags, before, after;
        __asm {
            mov before, esp
            mov ecx, 11223344h
            mov edx, 55667788h
            clc
            push 41
            mov eax, code
            call eax
            mov result, eax
            mov ecxResult, ecx
            mov edxResult, edx
            pushfd
            pop flags
            mov after, esp
        }
        CHECK(result == (redirect ? 777u : 42u), "runtime stub replays or skips exactly the original span");
        CHECK(ecxResult == 123 && edxResult == 456, "runtime stub restores handler-modified registers");
        CHECK(before == after, "runtime redirect and pass-through retain callee stack cleanup");
        CHECK((flags & 1) == (redirect ? 1u : 0u),
              "runtime redirect retains flags; replay retains native arithmetic flags");
    }
    VirtualFree(code, 0, MEM_RELEASE);
}
} // namespace runtime_stub_check
