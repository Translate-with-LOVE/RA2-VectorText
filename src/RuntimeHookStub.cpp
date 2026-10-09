// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "RuntimeHooks.h"
#include <cstring>

namespace vt::RuntimeHooks
{
std::optional<std::size_t> BuildStub(CodeBuffer buffer, DWORD origin, unsigned int span,
                                   const unsigned char *original, Handler handler)
{
    if (!buffer.data || !handler || (span && !original) || buffer.capacity < code::stubOverhead ||
        span > buffer.capacity - code::stubOverhead)
        return std::nullopt;
    auto *at = buffer.data;
    const auto emit = [&](const void *bytes, std::size_t size) {
        std::memcpy(at, bytes, size);
        at += size;
    };
    emit(code::snapshot, sizeof(code::snapshot));
    emit(&origin, sizeof(origin));
    emit(code::call, sizeof(code::call));
    const auto handlerAddress = reinterpret_cast<DWORD>(handler);
    emit(&handlerAddress, sizeof(handlerAddress));
    emit(code::branch, sizeof(code::branch));
    auto *displacement = at - 1;
    emit(code::redirect, sizeof(code::redirect));
    *displacement = static_cast<unsigned char>(sizeof(code::redirect));
    emit(code::restore, sizeof(code::restore));
    // Supported spans contain whole instructions with no PC-relative operands.
    if (span)
        emit(original, span);
    *at++ = 0xE9;
    const DWORD relative = origin + span - reinterpret_cast<DWORD>(at) - sizeof(DWORD);
    emit(&relative, sizeof(relative)); // memcpy avoids unaligned typed stores
    return static_cast<std::size_t>(at - buffer.data);
}
} // namespace vt::RuntimeHooks
