// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SyringeABI.h"
#include "RuntimeHookCode.h"
#include <optional>

namespace vt::RuntimeHooks
{
// Startup only, under the loader lock. Peers are held suspended throughout
// validation, commit and rollback; a peer inside a replaced span is rejected.
[[nodiscard]] bool Install(bool ra2);
using Handler = DWORD(__cdecl *)(REGISTERS *);
struct CodeBuffer { unsigned char *data; std::size_t capacity; };
constexpr std::size_t StubSize(std::size_t span) { return code::stubOverhead + span; }
// Capacity is checked before any byte is emitted. nullopt leaves the buffer intact.
[[nodiscard]] std::optional<std::size_t> BuildStub(CodeBuffer buffer, DWORD origin, unsigned int span,
                                                 const unsigned char *original, Handler handler);
} // namespace vt::RuntimeHooks
