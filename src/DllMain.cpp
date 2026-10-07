// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
// ===========================================================================
//  VectorText -- DLL entry point and (optional) Syringe handshake
//
//  Loading: Syringe reads the ".syhks00" section emitted by VT_DEFINE_HOOK;
//  no .inj file is needed. All hooks target gamemd.exe 1.001's own text
//  functions. Neither Phobos nor Ares is required for loading or rendering.
// ===========================================================================

#include "SyringeABI.h"
#include "Logger.h"
#include "Presentation32.h"

#include <string.h>
#include <stdio.h>

namespace
{
    void WriteMessage(SyringeHandshakeInfo* pInfo, const char* text)
    {
        if (!pInfo || !pInfo->Message || pInfo->cchMessage <= 0)
            return;
        strncpy_s(pInfo->Message, (size_t)pInfo->cchMessage, text, _TRUNCATE);
    }
}

// Syringe calls this once, before the game starts, when the export exists.
// IMPORTANT: this runs inside *Syringe's* process (Syringe LoadLibrary's the DLL
// to talk to it), not in gamemd.exe.  The hook handlers run in gamemd.exe, so
// the log is opened there, lazily, on the first intercepted call -- that is why
// nothing is written from here except the status message Syringe prints.
extern "C" __declspec(dllexport) HRESULT __cdecl SyringeHandshake(SyringeHandshakeInfo* pInfo)
{
    WriteMessage(pInfo, "VectorText: native text hooks and vector rendering.");
    return S_OK;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        vt::Log::Prepare();     // lock only; no file I/O under the loader lock
        vt::Presentation32::PrepareEarly();
        break;

    case DLL_PROCESS_DETACH:
        // Best effort: the periodic flush already wrote the statistics; this
        // only adds the final table (and does nothing if the process died).
        vt::Log::Shutdown();
        break;

    default:
        break;
    }
    return TRUE;
}
