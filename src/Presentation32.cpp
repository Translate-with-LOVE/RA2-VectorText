// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "presentation/State.h"

// cnc-ddraw detection and hook installation. Backends live in presentation/.
namespace vt::Presentation32::detail
{
FARPROC WINAPI HookProc(HMODULE module, LPCSTR name)
{
    FARPROC result = realProc(module, name);
    if (!FromCnc(_ReturnAddress()) || (uintptr_t)name <= 0xFFFF)
        return result;
    if (!strcmp(name, "Direct3DCreate9"))
    {
        return ResolveD3D9Create(result);
    }
    return result;
}
bool Detour(HMODULE module, const char *name, void *replacement, void **original)
{
    if (!module)
        return false;
    void *target = (void *)realProc(module, name);
    if (!target || MH_CreateHook(target, replacement, original) != MH_OK)
        return false;
    return MH_QueueEnableHook(target) == MH_OK;
}
void Start(HMODULE module)
{
    if (state)
        return;
    state = new State;
    if (!Cfg::ConfigBool("Enabled", true) || !Cfg::ConfigBool("Present32", true) || Cfg::Mode() != Cfg::Mode_Draw)
        return;
    cncModule = module;
    auto dos = (IMAGE_DOS_HEADER *)module;
    auto nt = (IMAGE_NT_HEADERS *)((char *)module + dos->e_lfanew);
    cncEnd = (uintptr_t)module + nt->OptionalHeader.SizeOfImage;
    state->options.linear = Cfg::LinearBlend();
    state->options.antialias = Cfg::AntiAlias();
    state->options.gamma = Cfg::Gamma();
    state->options.outline = Cfg::Outline();
    state->options.outlineColor = Cfg::OutlineColor();
    state->autoTextScale = Cfg::HiDPI();
    state->profile = Cfg::ConfigBool("PresentProfile", false);
    state->options.highResolution = state->autoTextScale;
    if (MH_Initialize() != MH_OK)
    {
        Log::Note("Present32: MinHook initialization failed");
        return;
    }
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const bool proc = Detour(kernel, "GetProcAddress", (void *)HookProc, (void **)&realProc);
    const bool gdi = InstallGdi();
    HMODULE executable = GetModuleHandleW(nullptr);
    char filename[MAX_PATH]{};
    GetModuleFileNameA(executable, filename, MAX_PATH);
    const char *basename = strrchr(filename, '\\');
    if (!_stricmp(basename ? basename + 1 : filename, "gamemd.exe"))
    {
        const unsigned char prefix[] = {0x81, 0xEC, 0xB4, 0, 0, 0, 0x53, 0x56, 0x8B, 0xF1, 0x57};
        const unsigned char copyPrefix[] = {0x8B, 0x44, 0x24, 0x1C, 0x83, 0xEC, 0x20, 0x53, 0x56, 0x8B, 0xF1};
        auto exeDos = (IMAGE_DOS_HEADER *)executable;
        auto exeNt = (IMAGE_NT_HEADERS *)((char *)executable + exeDos->e_lfanew);
        const bool valid = (uintptr_t)executable == 0x400000 && exeNt->FileHeader.TimeDateStamp == 0x3BDF544E &&
                           *(uintptr_t *)0x7E85E4 == 0x4BB620 && *(uintptr_t *)0x7E8658 == 0x4C1AB0 &&
                           !memcmp((void *)0x4BB620, prefix, sizeof(prefix)) &&
                           !memcmp((void *)0x437350, copyPrefix, sizeof(copyPrefix));
        if (!valid || MH_CreateHook((void *)0x4BB620, (void *)HookGameFill, (void **)&realGameFill) != MH_OK ||
            MH_QueueEnableHook((void *)0x4BB620) != MH_OK ||
            MH_CreateHook((void *)0x437350, (void *)HookGameCopy, (void **)&realGameCopy) != MH_OK ||
            MH_QueueEnableHook((void *)0x437350) != MH_OK)
        {
            MH_Uninitialize();
            Log::Note("Present32: native fill/copy entry differs; retaining RGB565");
            return;
        }
        // Loading constructs a BSurface (vtable 7E2070) at 552D94..552DCD,
        // stores it at loading-object+60 and deletes it through vtable[0]
        // at 5543EC..5543F7. Scope this optional hook to that verified type.
        const unsigned char deletePrefix[] = {0x56, 0x8B, 0xF1, 0x8D, 0x4E, 0x14};
        if (*(uintptr_t *)0x7E2070 == 0x411650 && !memcmp((void *)0x411650, deletePrefix, sizeof(deletePrefix)) &&
            MH_CreateHook((void *)0x411650, (void *)HookCpuDelete, (void **)&realCpuDelete) == MH_OK &&
            MH_QueueEnableHook((void *)0x411650) == MH_OK)
            state->cpuTextReady = true;
        else
            Log::Note("Present32: BSurface lifetime hook unavailable; CPU text retains RGB565");
    }
    if (!InstallOpenGL())
    {
        MH_Uninitialize();
        return;
    }
    if (!proc || !gdi || MH_ApplyQueued() != MH_OK)
    {
        MH_Uninitialize();
        Log::Note("Present32: detour setup failed; retaining RGB565");
        return;
    }
    state->enabled = true;
    SetPresentationWriter(Draw);
    Log::Note("Present32: cnc-ddraw detected; waiting for BGRA8 presenter");
}
HRESULT WINAPI HookCreateDD(GUID *guid, void **out, IUnknown *outer)
{
    HMODULE module = GetModuleHandleW(L"ddraw.dll");
    // Two distinctive exports, not merely a filename: native DirectDraw
    // and other wrappers must retain the existing rendering path.
    if (module && importedProc(module, "pvBmpBits") && importedProc(module, "GameHandlesClose"))
        Start(module);
    HRESULT hr = realCreate(guid, out, outer);
    if (SUCCEEDED(hr) && out && state && state->enabled)
    {
        Patch(*out, 0, (void *)HookDDQuery);
        Patch(*out, 6, (void *)HookCreateSurface);
    }
    return hr;
}
FARPROC WINAPI EarlyProc(HMODULE module, LPCSTR name)
{
    FARPROC result = importedProc(module, name);
    if ((uintptr_t)name > 0xFFFF && !strcmp(name, "DirectDrawCreate") && importedProc(module, "pvBmpBits") &&
        importedProc(module, "GameHandlesClose"))
    {
        realCreate = (CreateDD)result;
        return (FARPROC)HookCreateDD;
    }
    return result;
}
void Import(void **slot, void *replacement)
{
    DWORD protection;
    if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection))
    {
        InterlockedExchangePointer(slot, replacement);
        DWORD ignored;
        VirtualProtect(slot, sizeof(void *), protection, &ignored);
    }
}
} // namespace vt::Presentation32::detail

namespace vt::Presentation32
{
using namespace detail;
void PrepareEarly()
{
    // Syringe temporarily loads this DLL in its own process to read the
    // handshake, then unloads it. Never leave imports pointing into it.
#ifndef VT_PRESENT_TEST
    char executable[MAX_PATH]{};
    GetModuleFileNameA(nullptr, executable, MAX_PATH);
    const char *filename = strrchr(executable, '\\');
    if (_stricmp(filename ? filename + 1 : executable, "gamemd.exe"))
        return;
#endif
    auto base = (unsigned char *)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER *)base;
    auto nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva)
        return;
    auto descriptor = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva);
    for (; descriptor->Name; ++descriptor)
    {
        if (!descriptor->OriginalFirstThunk)
            continue;
        auto names = (IMAGE_THUNK_DATA *)(base + descriptor->OriginalFirstThunk);
        auto addresses = (IMAGE_THUNK_DATA *)(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const char *name = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            void **slot = (void **)&addresses->u1.Function;
            if (!strcmp(name, "DirectDrawCreate"))
            {
                realCreate = (CreateDD)*slot;
                Import(slot, (void *)HookCreateDD);
            }
            else if (!strcmp(name, "GetProcAddress"))
            {
                importedProc = (Proc)*slot;
                Import(slot, (void *)EarlyProc);
            }
        }
    }
}
void TrackTextSurface(void *surface)
{
    if (!state || !state->enabled || !state->cpuTextReady || !surface)
        return;
    // Only the verified native BSurface class owns this memory layout.
    // All DSurface and extension surface paths keep their existing hooks.
    if (*(uintptr_t *)surface != 0x7E2070)
        return;
    TrackCpuText(surface);
}
Statistics Stats()
{
    if (!state)
        return {};
    Guard guard;
    return state->stats;
}
} // namespace vt::Presentation32
