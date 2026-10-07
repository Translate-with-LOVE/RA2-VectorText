// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "State.h"

// GDI BGRA8 DIB conversion; preserve the native destination rectangle.
namespace vt::Presentation32::detail
{
// GDI keeps its destination rectangle and source orientation, but consumes
// a 32-bit DIB. These detours are strictly scoped to cnc-ddraw callers.
using Stretch = int(WINAPI *)(HDC, int, int, int, int, int, int, int, int, const void *, const BITMAPINFO *, UINT,
                              DWORD);
using SetDIB = int(WINAPI *)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT, const void *, const BITMAPINFO *, UINT);
Stretch realStretch = nullptr;
SetDIB realSetDIB = nullptr;
bool Dib(const void *pixels, const BITMAPINFO *info, std::vector<uint32_t> &out, BITMAPINFO &converted)
{
    if (!info || info->bmiHeader.biBitCount != 16 || info->bmiHeader.biWidth <= 0)
        return false;
    Guard guard;
    const auto found = state->buffers.find((void *)pixels);
    if (found == state->buffers.end())
        return false;
    auto b = found->second;
    b->plane.ValidateNative(b->base, b->pitch);
    if (info->bmiHeader.biWidth != b->width || abs(info->bmiHeader.biHeight) != b->height)
        return false;
    std::vector<uint32_t> top((size_t)b->width * b->height);
    b->plane.CompositeRect(b->base, b->pitch, top.data(), b->width, {0, 0, b->width, b->height});
    RecordFrame(top.data(), b->width, b->height, b->width);
    out.resize(top.size());
    for (int y = 0; y < b->height; ++y)
        memcpy(out.data() + (size_t)y * b->width,
               top.data() + (size_t)(info->bmiHeader.biHeight > 0 ? b->height - 1 - y : y) * b->width,
               (size_t)b->width * 4);
    converted = {};
    converted.bmiHeader = info->bmiHeader;
    converted.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    converted.bmiHeader.biBitCount = 32;
    converted.bmiHeader.biCompression = BI_RGB;
    converted.bmiHeader.biClrUsed = 0;
    converted.bmiHeader.biClrImportant = 0;
    converted.bmiHeader.biSizeImage = (DWORD)out.size() * 4;
    state->stats.backend = 1;
    return true;
}
int WINAPI HookStretch(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh, const void *bits,
                       const BITMAPINFO *bmi, UINT usage, DWORD rop)
{
    std::vector<uint32_t> out;
    BITMAPINFO info{};
    if (FromCnc(_ReturnAddress()) && usage == DIB_RGB_COLORS && Dib(bits, bmi, out, info))
        return realStretch(dc, x, y, w, h, sx, sy, sw, sh, out.data(), &info, usage, rop);
    return realStretch(dc, x, y, w, h, sx, sy, sw, sh, bits, bmi, usage, rop);
}
int WINAPI HookSetDIB(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT start, UINT lines, const void *bits,
                      const BITMAPINFO *bmi, UINT usage)
{
    std::vector<uint32_t> out;
    BITMAPINFO info{};
    if (FromCnc(_ReturnAddress()) && usage == DIB_RGB_COLORS && Dib(bits, bmi, out, info))
        return realSetDIB(dc, x, y, w, h, sx, sy, start, lines, out.data(), &info, usage);
    return realSetDIB(dc, x, y, w, h, sx, sy, start, lines, bits, bmi, usage);
}

bool InstallGdi()
{
    HMODULE module = GetModuleHandleW(L"gdi32.dll");
    const bool stretch = Detour(module, "StretchDIBits", (void *)HookStretch, (void **)&realStretch);
    const bool set = Detour(module, "SetDIBitsToDevice", (void *)HookSetDIB, (void **)&realSetDIB);
    return stretch && set;
}
} // namespace vt::Presentation32::detail
