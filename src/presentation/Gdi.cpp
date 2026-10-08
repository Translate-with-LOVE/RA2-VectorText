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
static decltype(&CreateCompatibleDC) createDC;
static decltype(&DeleteDC) deleteDC;
static decltype(&CreateDIBSection) createDIB;
static decltype(&SelectObject) selectObject;
static decltype(&DeleteObject) deleteObject;
static decltype(&GetStretchBltMode) getStretchMode;
static decltype(&SetStretchBltMode) setStretchMode;
static decltype(&GdiFlush) flush;
struct Canvas
{
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t *pixels = nullptr;
    int width = 0, height = 0;
    ~Canvas() { Reset(); }
    void Reset()
    {
        if (old)
            selectObject(dc, old);
        if (bitmap)
            deleteObject(bitmap);
        if (dc)
            deleteDC(dc);
        dc = nullptr;
        bitmap = nullptr;
        old = nullptr;
        pixels = nullptr;
        width = height = 0;
    }
    bool Resize(int w, int h)
    {
        if (pixels && w == width && h == height)
            return true;
        Reset();
        if (w <= 0 || h <= 0 || (size_t)w * h > 64u * 1024u * 1024u)
            return false;
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        dc = createDC(nullptr);
        if (dc)
            bitmap = createDIB(dc, &info, DIB_RGB_COLORS, (void **)&pixels, nullptr, 0);
        if (!bitmap)
        {
            Reset();
            return false;
        }
        old = selectObject(dc, bitmap);
        width = w;
        height = h;
        return true;
    }
};
bool Dib(const void *pixels, const BITMAPINFO *info, std::vector<uint32_t> &out, BITMAPINFO &converted,
         std::unique_ptr<PixelPlane> *snapshot = nullptr)
{
    if (!info || info->bmiHeader.biBitCount != 16 || info->bmiHeader.biWidth <= 0)
        return false;
    Guard guard;
    const auto found = state->buffers.find((void *)pixels);
    if (found == state->buffers.end())
        return false;
    auto b = found->second;
    if (info->bmiHeader.biWidth != b->width || abs(info->bmiHeader.biHeight) != b->height)
        return false;
    b->plane.ValidateNative(b->base, b->pitch);
    const PixelPlane frame(b->plane);
    std::vector<uint32_t> top((size_t)b->width * b->height);
    if (snapshot)
    {
        *snapshot = std::make_unique<PixelPlane>(frame);
        frame.BackgroundRect(b->base, b->pitch, top.data(), b->width, {0, 0, b->width, b->height});
    }
    else
        frame.CompositeRect(b->base, b->pitch, top.data(), b->width, {0, 0, b->width, b->height});
    // Test/profile accounting keeps its logical-resolution contract.
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
    if (!FromCnc(_ReturnAddress()) || usage != DIB_RGB_COLORS)
        return realStretch(dc, x, y, w, h, sx, sy, sw, sh, bits, bmi, usage, rop);
    std::vector<uint32_t> out;
    BITMAPINFO info{};
    const bool scaled = state->autoTextScale && rop == SRCCOPY && sw > 0 && sh > 0 && w > sw && h > sh;
    std::unique_ptr<PixelPlane> frame;
    if (scaled && Dib(bits, bmi, out, info, &frame))
    {
        const int sourceY = info.bmiHeader.biHeight > 0 ? frame->Height() - sy - sh : sy;
        const PixelRect source{sx, sourceY, sx + sw, sourceY + sh};
        static thread_local Canvas canvas;
        if (source.left >= 0 && source.top >= 0 && source.right <= frame->Width() && source.bottom <= frame->Height() &&
            canvas.Resize(w, h))
        {
            ObserveOutput(w / (float)sw, h / (float)sh, frame->RasterScale());
            setStretchMode(canvas.dc, getStretchMode(dc));
            const int copied = realStretch(canvas.dc, 0, 0, w, h, sx, sy, sw, sh, out.data(), &info, usage, rop);
            flush(); // Finish GDI's background writes before software touches the DIB.
            if (copied && copied != GDI_ERROR)
            {
                frame->CompositeScaledRect(canvas.pixels, w, w, h, source);
                BITMAPINFO finalInfo{};
                finalInfo.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
                const int result = realStretch(dc, x, y, w, h, 0, 0, w, h, canvas.pixels, &finalInfo, usage, SRCCOPY);
                Guard guard;
                ++state->stats.overlays;
                return result;
            }
        }
        // An allocation or unsupported crop keeps the complete logical text.
        out.clear();
    }
    ObserveOutput(0, 0, 0);
    if (Dib(bits, bmi, out, info))
        return realStretch(dc, x, y, w, h, sx, sy, sw, sh, out.data(), &info, usage, rop);
    return realStretch(dc, x, y, w, h, sx, sy, sw, sh, bits, bmi, usage, rop);
}
int WINAPI HookSetDIB(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT start, UINT lines, const void *bits,
                      const BITMAPINFO *bmi, UINT usage)
{
    std::vector<uint32_t> out;
    BITMAPINFO info{};
    if (FromCnc(_ReturnAddress()) && usage == DIB_RGB_COLORS && Dib(bits, bmi, out, info))
    {
        ObserveOutput(0, 0, 0);
        return realSetDIB(dc, x, y, w, h, sx, sy, start, lines, out.data(), &info, usage);
    }
    return realSetDIB(dc, x, y, w, h, sx, sy, start, lines, bits, bmi, usage);
}

bool InstallGdi()
{
    HMODULE module = GetModuleHandleW(L"gdi32.dll");
#define LOAD_GDI(variable, name)                                                                                       \
    variable = (decltype(variable))realProc(module, name);                                                             \
    if (!variable)                                                                                                     \
    return false
    LOAD_GDI(createDC, "CreateCompatibleDC");
    LOAD_GDI(deleteDC, "DeleteDC");
    LOAD_GDI(createDIB, "CreateDIBSection");
    LOAD_GDI(selectObject, "SelectObject");
    LOAD_GDI(deleteObject, "DeleteObject");
    LOAD_GDI(getStretchMode, "GetStretchBltMode");
    LOAD_GDI(setStretchMode, "SetStretchBltMode");
    LOAD_GDI(flush, "GdiFlush");
#undef LOAD_GDI
    const bool stretch = Detour(module, "StretchDIBits", (void *)HookStretch, (void **)&realStretch);
    const bool set = Detour(module, "SetDIBitsToDevice", (void *)HookSetDIB, (void **)&realSetDIB);
    return stretch && set;
}
} // namespace vt::Presentation32::detail
