// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SyringeABI.h"
#include "YRAddresses.h"
#include "Presentation32.h"
#include "Logger.h"
#include "Takeover.h"
#include "PixelWriter.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

// Private safe reads and text observation; handlers remain thin ABI adapters.
namespace vt::hooks
{
struct TextArg
{
    const wchar_t *ptr;
    int offset; // 0 when the value did not come from the stack
    bool fromReg;
};

bool RangeOk(const void *p, size_t bytes);
void ReportText(int hookId, REGISTERS *R, TextArg arg, int nArgs, int firstArgOffset, const char *what);
} // namespace vt::hooks
