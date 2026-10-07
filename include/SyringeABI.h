// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ===========================================================================
//  Syringe hook ABI  (Syringe 0.7.x -- this install ships 0.7.3.0 "mo")
// ===========================================================================
//
//  A hook handler is an exported __cdecl function:
//
//      extern "C" __declspec(dllexport) DWORD __cdecl Handler(REGISTERS* R);
//
//  Return value:
//      0        -> Syringe replays the original `size` bytes it overwrote,
//                  then continues at hookAddr + size.  ("observe and pass on")
//      nonzero  -> Syringe jumps to that address instead; the register block
//                  may be modified beforehand to change arguments/results.
//
//  Registering hooks: either an external "<dllname>.inj" text file
//  (ADDRESS = ExportName, SizeInHex) or a ".syhks00" section embedded in the
//  DLL.  This project uses the section, exactly like Phobos does.  The section
//  holds 16-byte aligned records of
//
//      struct hookdecl { u32 hookAddr; u32 hookSize; const char* hookName; };
//
//  where hookName must equal the name of an exported function of this DLL
//  (Ares/Phobos export every hook handler for this reason).
//
//  The declarations below are our own; the field order of REGISTERS and the
//  record layout are the ABI, and are kept identical to Syringe's header
//  (Syringe.h by pd, shipped with YRpp).  Do not reorder REGISTERS.
// ===========================================================================

#ifndef SYR_VER
#define SYR_VER 2
#endif

struct REGISTERS
{
    DWORD origin;       // address this hook was placed at
    DWORD flags;        // EFLAGS at hook time
    DWORD edi, esi, ebp, esp, ebx, edx, ecx, eax;

    DWORD Origin() const { return origin; }
    DWORD EFLAGS() const { return flags; }
    void  EFLAGS(DWORD v) { flags = v; }

    DWORD EAX() const { return eax; }
    DWORD EBX() const { return ebx; }
    DWORD ECX() const { return ecx; }
    DWORD EDX() const { return edx; }
    DWORD ESI() const { return esi; }
    DWORD EDI() const { return edi; }
    DWORD ESP() const { return esp; }
    DWORD EBP() const { return ebp; }

    void EAX(DWORD v) { eax = v; }
    void EBX(DWORD v) { ebx = v; }
    void ECX(DWORD v) { ecx = v; }
    void EDX(DWORD v) { edx = v; }
    void ESP(DWORD v) { esp = v; }        // needed to skip a callee (pop args + ret)

    // Stack access relative to the ESP captured at the hook address.
    // At a function's first instruction ESP+0 is the return address and
    // ESP+4 is the first stack argument (cdecl / stdcall / thiscall alike);
    // for __thiscall the object pointer lives in ECX.
    template <typename T>
    T Stack(int offset) const
    {
        return *reinterpret_cast<const T*>(static_cast<uintptr_t>(esp) + offset);
    }

    DWORD Stack32(int offset) const { return Stack<DWORD>(offset); }
};

#if SYR_VER == 2

#pragma pack(push, 16)
struct __declspec(align(16)) hookdecl
{
    unsigned int hookAddr;
    unsigned int hookSize;
    const char*  hookName;
};
#pragma pack(pop)

#pragma section(".syhks00", read, write)

#define VT_HOOK_DECL(addr, funcname, size)                                      \
    namespace SyringeData { namespace Hooks {                                   \
        __declspec(allocate(".syhks00")) hookdecl _hk_##funcname =               \
            { (unsigned int)(addr), (unsigned int)(size), #funcname };           \
    } }

#define VT_HOOK_FUNC(funcname)                                                  \
    extern "C" __declspec(dllexport) DWORD __cdecl funcname(REGISTERS* R)

#define VT_DEFINE_HOOK(addr, funcname, size)                                    \
    VT_HOOK_DECL(addr, funcname, size)                                          \
    VT_HOOK_FUNC(funcname)

#endif // SYR_VER == 2

// Handshake: optional.  When this export exists Syringe calls it once per DLL
// and prints the message; Phobos does not export it, Ares does.
struct SyringeHandshakeInfo
{
    int          cbSize;        // sizeof(SyringeHandshakeInfo) as Syringe knows it
    int          num_hooks;     // hooks Syringe found for this DLL
    unsigned int checksum;      // host checksum
    DWORD        exeFilesize;
    DWORD        exeTimestamp;
    unsigned int exeCRC;
    int          cchMessage;    // capacity of Message (chars, incl. terminator)
    char*        Message;       // write a status line here (optional)
};

typedef HRESULT(__cdecl* SyringeHandshakeProc)(SyringeHandshakeInfo* pInfo);
