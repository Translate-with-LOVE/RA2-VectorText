// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "RuntimeHooks.h"
#include <cstring>

namespace vt::RuntimeHooks
{
namespace
{
void Jump(unsigned char *at, const void *target)
{
    at[0] = 0xE9;
    *(DWORD *)(at + 1) = (DWORD)target - (DWORD)at - 5;
}
} // namespace

unsigned int BuildStub(unsigned char *code, DWORD origin, unsigned int span, const unsigned char *original,
                       DWORD(__cdecl *handler)(REGISTERS *))
{
    unsigned char *at = code;
    const unsigned char snapshot[] = {0x9C, 0x60,                   // pushfd; pushad
                                      0x83, 0x44, 0x24, 0x0C, 0x04, // captured ESP is before pushfd
                                      0x83, 0xEC, 0x08,             // REGISTERS origin/flags
                                      0x8B, 0x44, 0x24, 0x28, 0x89, 0x44, 0x24, 0x04, 0xC7, 0x04, 0x24};
    memcpy(at, snapshot, sizeof(snapshot));
    at += sizeof(snapshot);
    *(DWORD *)at = origin;
    at += 4;
    *at++ = 0xFC; // cld for the C++ handler; flags restored below
    *at++ = 0x54; // REGISTERS*
    *at++ = 0xB8;
    *(DWORD *)at = (DWORD)handler;
    at += 4;
    *at++ = 0xFF;
    *at++ = 0xD0;
    const unsigned char branch[] = {0x83, 0xC4, 0x04, 0x85, 0xC0, 0x74, 0x00};
    memcpy(at, branch, sizeof(branch));
    at += sizeof(branch);
    unsigned char *displacement = at - 1;
    // Nonzero handler return: place the jump destination in the old EFLAGS
    // slot, restore the handler's flags/registers, then RET with original ESP.
    // No shared scratch register, TLS slot or arbitrary TIB pointer is used.
    const unsigned char redirect[] = {0x89, 0x44, 0x24, 0x28, 0xFF, 0x74, 0x24, 0x04,
                                      0x9D, 0x8D, 0x64, 0x24, 0x08, 0x61, 0xC3};
    memcpy(at, redirect, sizeof(redirect));
    at += sizeof(redirect);
    *displacement = (unsigned char)(at - displacement - 1);
    const unsigned char restore[] = {0x8B, 0x44, 0x24, 0x04, 0x89, 0x44, 0x24, 0x28, 0x83, 0xC4, 0x08, 0x61, 0x9D};
    memcpy(at, restore, sizeof(restore));
    at += sizeof(restore);
    // Every supported span is whole instructions and contains no PC-relative
    // operand. The exact bytes for both builds are checked before installation.
    memcpy(at, original, span);
    at += span;
    Jump(at, (void *)(origin + span));
    at += 5;
    return (unsigned int)(at - code);
}

} // namespace vt::RuntimeHooks
