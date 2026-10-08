// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SyringeABI.h"

namespace vt::RuntimeHooks
{
// Called only during initial DLL loading, while Syringe suspends the game.
bool Install(bool ra2);
// Shared with the machine-code regression test. Returns the emitted byte count.
unsigned int BuildStub(unsigned char *code, DWORD origin, unsigned int span, const unsigned char *original,
                       DWORD(__cdecl *handler)(REGISTERS *));
} // namespace vt::RuntimeHooks
