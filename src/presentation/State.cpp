// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "State.h"

// Shared state, COM detours, frame diagnostics and glyph-writer entry.
namespace vt::Presentation32::detail
{
State *state = nullptr; // Process lifetime: no STL destruction under loader lock.
HMODULE cncModule = nullptr;
uintptr_t cncEnd = 0;
Proc realProc = ::GetProcAddress, importedProc = ::GetProcAddress;
CreateDD realCreate = nullptr;
LONGLONG Stamp()
{
    LARGE_INTEGER tick{};
    if (state->profile)
        QueryPerformanceCounter(&tick);
    return tick.QuadPart;
}
double Elapsed(LONGLONG start)
{
    return state->profile ? (Stamp() - start) * 1000.0 / state->frequency.QuadPart : 0;
}
bool FromCnc(void *caller)
{
    return cncModule && (uintptr_t)caller >= (uintptr_t)cncModule && (uintptr_t)caller < cncEnd;
}
void *Original(void *object, int slot)
{
    void **table = *(void ***)object;
    Guard guard;
    for (const auto &s : state->slots)
        if (s.table == table && s.index == slot)
            return s.original;
    return table[slot];
}
bool Patch(void *object, int index, void *replacement)
{
    if (!object)
        return false;
    void **table = *(void ***)object;
    Guard guard;
    for (const auto &s : state->slots)
        if (s.table == table && s.index == index)
            return table[index] == replacement;
    DWORD protection;
    if (!VirtualProtect(table + index, sizeof(void *), PAGE_EXECUTE_READWRITE, &protection))
        return false;
    state->slots.push_back({table, index, table[index]});
    InterlockedExchangePointer(table + index, replacement);
    DWORD ignored;
    VirtualProtect(table + index, sizeof(void *), protection, &ignored);
    return true;
}
int Draw(const Target &t, const GlyphCell &cell, int x, int y, int rows, unsigned short color, bool aa)
{
    if (!state || !state->enabled || !t.base)
        return -1;
    Guard guard;
    if (aa != state->options.antialias)
        return -1;
    const auto found = state->buffers.find(t.base);
    if (found == state->buffers.end() || found->second->pitch != t.pitch)
        return -1;
    // Both the primary startup image and the cached CPU loading image can
    // be painted before any presenter runs (especially GDI). Retain them
    // immediately; compatibility pixels cover the interval until it is ready.
    const auto start = Stamp();
    found->second->plane.Paint(cell, x, y, rows, color, {t.clipL, t.clipT, t.clipR + 1, t.clipB + 1},
                               found->second->base, found->second->pitch, SubtitleOutlineActive());
    static LONG reported=0;
    const LONG density=cell.raster2 ? cell.raster2->scale : 1;
    if(InterlockedExchange(&reported,density)!=density)
        Log::Note("Present32: glyph retained raster=%ld base=%p pitch=%d position=(%d,%d)",density,t.base,t.pitch,x,y);
    state->stats.glyphMs += Elapsed(start);
    ++state->stats.glyphs;
    return 1;
}
void Disable(const char *reason)
{
    Guard guard;
    // If a presenter fails after text was already retained, materialize
    // that text once before declining future glyphs. Never drop it.
    for (auto &entry : state->buffers)
    {
        auto &b = *entry.second;
        const auto pixels = b.plane.CaptureNative(b.base, b.pitch, {0, 0, b.width, b.height});
        for (const auto &p : pixels)
        {
            uint32_t rgb = b.plane.Composite(p.x, p.y, p.base);
            b.base[(size_t)p.y * b.pitch + p.x] =
                (unsigned short)((((rgb >> 19) & 31) << 11) | (((rgb >> 10) & 63) << 5) | ((rgb >> 3) & 31));
        }
        b.plane.Clear({0, 0, b.width, b.height});
    }
    state->enabled = false;
    state->stats.backend = 0;
    SetPresentationWriter(nullptr);
    Log::Note("Present32: %s; retaining RGB565", reason);
}
void RecordFrame(const uint32_t *pixels, int width, int height, int pitch)
{
    ++state->stats.frames;
    if (state->profile && state->stats.frames % 300 == 0)
        Log::Note(
            "Present32 perf: frames=%ld glyph=%.3fms/frame copy=%.3fms/frame upload=%.3fms/upload overlay=%.3fms/draw",
            state->stats.frames, state->stats.glyphMs / state->stats.frames, state->stats.copyMs / state->stats.frames,
            state->stats.uploadMs / std::max(1L, state->stats.uploads),
            state->stats.overlayMs / std::max(1L, state->stats.overlays));
#ifdef VT_PRESENT_TEST
    state->lastWidth = width;
    state->lastHeight = height;
    state->last.resize((size_t)width * height);
    for (int y = 0; y < height; ++y)
        memcpy(state->last.data() + (size_t)y * width, pixels + (size_t)y * pitch, (size_t)width * 4);
#else
    (void)pixels;
    (void)width;
    (void)height;
    (void)pitch;
#endif
}
bool ObserveOutput(float sx, float sy, int raster)
{
    const bool scaled = state->autoTextScale && std::isfinite(sx) && std::isfinite(sy) && sx > 1.001f && sy > 1.001f;
    Guard guard;
    state->outputObserved = true;
    state->outputScaleX = scaled ? sx : 0;
    state->outputScaleY = scaled ? sy : 0;
    state->outputRaster = scaled ? raster : 0;
    if (scaled)
        SetOutputRasterScale((int)std::ceil(std::min(8.0f, std::max(sx, sy)) - 0.0001f));
    return scaled;
}
} // namespace vt::Presentation32::detail
