// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <windows.h>
#include <cstdio>

int main()
{
    HMODULE module = LoadLibraryA("VectorText.dll");
    const DWORD error = GetLastError();
    if (module)
        FreeLibrary(module);
    const bool rejected = !module && error == ERROR_DLL_INIT_FAILED;
    printf("unknown executable rejected before any hook: %s (error=%lu)\n", rejected ? "PASS" : "FAIL", error);
    return rejected ? 0 : 1;
}
