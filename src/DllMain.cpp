// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
// ===========================================================================
//  VectorText -- DLL entry point and (optional) Syringe handshake
//
//  Syringe discovers the common bootstrap in .syhks00. The DLL selects and
//  verifies RA2/YR text addresses before installing any runtime text hook.
// ===========================================================================

#include "SyringeABI.h"
#include "Logger.h"
#include "Presentation32.h"
#include "GameAddresses.h"
#include "RuntimeHooks.h"
#include "Takeover.h"

#include <string.h>
#include <stdio.h>

namespace
{
    bool gameHost = false;
    bool IsCompatibleHost()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(NULL, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        const auto module = (const unsigned char*)GetModuleHandleA(NULL);
        const auto dos = (const IMAGE_DOS_HEADER*)module;
        const auto pe = (const IMAGE_NT_HEADERS32*)(module + dos->e_lfanew);
        const DWORD stamp = pe->FileHeader.TimeDateStamp;
        if (stamp != yra::kExeTimestamp && stamp != ra2a::kExeTimestamp)
            return !_stricmp(name, "Syringe.exe") || !_stricmp(name, "SyringeEx.exe") ||
                   GetProcAddress((HMODULE)module, "VT_OfflineTestHost") != NULL;
        if ((uintptr_t)module != 0x400000 || pe->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
            return false;
        const bool ra2 = stamp == ra2a::kExeTimestamp;
        const DWORD lastGlobal = ra2 ? ra2a::BitFont_Instance : yra::BitFont_Instance;
        if (pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            pe->OptionalHeader.SizeOfImage < lastGlobal - 0x400000 + sizeof(void*))
            return false;
        const unsigned char bootstrap[] = {0x53, 0x56, 0x57, 0x8B, 0xF1};
        if (memcmp((void*)0x401000, bootstrap, sizeof(bootstrap)))
            return false;
        game::Select(ra2);
        gameHost = true;
        return vt::RuntimeHooks::Install(ra2);
    }

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
    if (!pInfo || pInfo->cbSize < sizeof(SyringeHandshakeInfo))
        return S_FALSE;
    if (pInfo->exeTimestamp != yra::kExeTimestamp && pInfo->exeTimestamp != ra2a::kExeTimestamp)
    {
        WriteMessage(pInfo, "VectorText: unsupported executable for this DLL; hooks declined.");
        return S_FALSE;
    }
    if (!gameHost)
        game::Select(pInfo->exeTimestamp == ra2a::kExeTimestamp);
    WriteMessage(pInfo, game::Version);
    return S_OK;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!IsCompatibleHost())
            return FALSE;
        DisableThreadLibraryCalls(hModule);
        vt::Log::Prepare();     // lock only; no file I/O under the loader lock
        vt::Presentation32::PrepareEarly();
        break;

    case DLL_PROCESS_DETACH:
    {
        // Best effort: periodic summaries contain hook counts and observed text.
        // Shutdown adds final counts and takeover diagnostics; abrupt process
        // termination may bypass this detach path entirely.
        char summary[2048]{};
        vt::Takeover::ShutdownSummary(summary, sizeof(summary));
        vt::Log::Shutdown(summary);
        break;
    }

    default:
        break;
    }
    return TRUE;
}
