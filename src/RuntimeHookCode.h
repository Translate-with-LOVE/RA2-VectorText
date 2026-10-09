// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>

namespace vt::RuntimeHooks::code
{
inline constexpr unsigned char snapshot[] = {
    0x9C, 0x60, 0x83, 0x44, 0x24, 0x0C, 0x04, 0x83, 0xEC, 0x08,
    0x8B, 0x44, 0x24, 0x28, 0x89, 0x44, 0x24, 0x04, 0xC7, 0x04, 0x24};
inline constexpr unsigned char call[] = {0xFC, 0x54, 0xB8};
inline constexpr unsigned char branch[] = {0xFF, 0xD0, 0x83, 0xC4, 0x04, 0x85, 0xC0, 0x74, 0x00};
inline constexpr unsigned char redirect[] = {
    0x89, 0x44, 0x24, 0x28, 0xFF, 0x74, 0x24, 0x04, 0x9D, 0x8D, 0x64, 0x24, 0x08, 0x61, 0xC3};
inline constexpr unsigned char restore[] = {
    0x8B, 0x44, 0x24, 0x04, 0x89, 0x44, 0x24, 0x28, 0x83, 0xC4, 0x08, 0x61, 0x9D};
inline constexpr std::size_t jumpSize = 1 + sizeof(unsigned long);
inline constexpr std::size_t stubOverhead = sizeof(snapshot) + sizeof(unsigned long) + sizeof(call) +
    sizeof(unsigned long) + sizeof(branch) + sizeof(redirect) + sizeof(restore) + jumpSize;
static_assert(sizeof(unsigned long) == 4, "The runtime hook encoding requires Windows x86.");
static_assert(sizeof(void *) == sizeof(unsigned long), "Runtime hooks require 32-bit pointers.");
static_assert(sizeof(redirect) <= 127, "The conditional branch must fit rel8.");
} // namespace vt::RuntimeHooks::code
