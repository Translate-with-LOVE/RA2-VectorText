// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Atlas.h"

// Test-only adapters; this translation unit is excluded from the DLL.
namespace vt::Presentation32
{
using namespace detail;
float TestObservedScale(bool vertical)
{
    if (!state)
        return 0;
    Guard guard;
    if(state->stats.backend!=2) return vertical ? state->outputScaleY : state->outputScaleX;
    for (const auto &entry : state->textures)
        if (entry.second.overlayReady && entry.second.cleanWorld)
            return vertical ? entry.second.reportedScaleY : entry.second.reportedScale;
    return 0;
}
int TestObservedRaster()
{
    if(!state) return 0;
    Guard guard;
    if(state->stats.backend!=2) return state->outputRaster;
    for(const auto& entry:state->textures)
        if(entry.second.overlayReady && entry.second.cleanWorld) return entry.second.rasterScale;
    return 0;
}
bool TestBuildOverlay(const PixelPlane &plane, float sx, float sy, float offsetX, float offsetY, TestOverlay &output,
                      const PixelRect *source)
{
    output = {};
    Texture t{};
    t.width = plane.Width();
    t.height = plane.Height();
    t.rasterScale=plane.RasterScale();
    const auto r = source ? *source : PixelRect{0, 0, t.width, t.height};
    const float l = offsetX - 0.5f, top = offsetY - 0.5f;
    const float right = l + (r.right - r.left) * sx, bottom = top + (r.bottom - r.top) * sy;
    const float u0 = (float)r.left / t.width, v0 = (float)r.top / t.height;
    const float u1 = (float)r.right / t.width, v1 = (float)r.bottom / t.height;
    ScreenVertex quad[4]{
        {l, bottom, 0, 1, u0, v1}, {l, top, 0, 1, u0, v0}, {right, bottom, 0, 1, u1, v1}, {right, top, 0, 1, u1, v0}};
    float actualX = 0, actualY = 0;
    if (!OutputScale(t, quad, actualX, actualY))
        return false;
    output.point = fabs(actualX - t.rasterScale) < 0.0001f && fabs(actualY - t.rasterScale) < 0.0001f;
    t.overlayTiles = plane.TextTiles(true);
    if (!AtlasShape(t.overlayTiles.size(), 4096, 4096, t.atlasWidth, t.atlasHeight, t.atlasColumns,t.rasterScale))
        return false;
    output.width = t.atlasWidth;
    output.height = t.atlasHeight;
    output.pixels.resize((size_t)output.width * output.height);
    PackOverlay(plane, t.overlayTiles, output.pixels.data(), output.width, t.atlasColumns,t.rasterScale);
    AtlasVertices(t, quad, actualX, actualY);
    for (const auto &v : t.overlayVertices)
        output.vertices.insert(output.vertices.end(), {v.x, v.y, v.u, v.v});
    return true;
}
void TestCpuTextStart()
{
    state = new State();
    state->enabled = true;
    state->cpuTextReady = true;
    state->autoTextScale = Cfg::HiDPI();
    state->options.highResolution = state->autoTextScale;
    // Deliberately leave backend=0: loading can precede texture creation.
    SetPresentationWriter(Draw);
}
HRESULT TestCooperativeLevel(void* directDraw, HWND window, DWORD flags)
{
    return HookCooperativeLevel(directDraw, window, flags);
}
void TestPrimaryTrack(void* surface)
{
    DDSURFACEDESC2 d{};
    if (Describe(surface, d))
    {
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        Guard guard;
        Remember(surface, d);
    }
}
int TestRetainedRaster(void* surface)
{
    Guard guard;
    const auto found = state->surfaces.find(surface);
    return found != state->surfaces.end() ? found->second.buffer->plane.RasterScale() : 0;
}
void TestOutputScale(float sx, float sy)
{
    ObserveOutput(sx, sy, OutputRasterScale());
}
void TestCpuTextTrack(void *surface)
{
    TrackCpuText(surface);
}
void TestCpuTextRelease(void *surface)
{
    ForgetCpuText(surface);
}
bool TestCpuTextOverlay(void *surface, unsigned int *output, int pitch)
{
    DDSURFACEDESC2 d{};
    void *native = nullptr;
    if (!GameDescription(surface, d, native))
        return false;
    Guard guard;
    const auto found = state->buffers.find(d.lpSurface);
    if (found == state->buffers.end())
        return false;
    auto &b = *found->second;
    b.plane.ValidateNative(b.base, b.pitch);
    b.plane.Overlay2Rect(output, pitch, {0, 0, b.width, b.height});
    return true;
}
unsigned int TestPixel(int x, int y)
{
    if (!state)
        return 0;
    Guard guard;
    return x >= 0 && y >= 0 && x < state->lastWidth && y < state->lastHeight
               ? state->last[(size_t)y * state->lastWidth + x]
               : 0;
}
bool TestFrameUploadBegin(void* surface,void* texture,void** pixels,int* pitch)
{
    DDSURFACEDESC2 d{};void* native=nullptr;
    if(!GameDescription(surface,d,native) || !native) return false;
    {
        Guard guard;
        Remember(native,d);state->primary=native;
        if(state->textures.find(texture)==state->textures.end()) {
            Texture t{};t.width=(int)d.dwWidth;t.height=(int)d.dwHeight;
            state->textures.emplace(texture,std::move(t));
        }
    }
    D3DLOCKED_RECT out{};RECT rect{0,0,(LONG)d.dwWidth,(LONG)d.dwHeight};
    const HRESULT hr=HookTextureLock(texture,0,&out,&rect,0);
    *pixels=out.pBits;*pitch=out.Pitch;return SUCCEEDED(hr);
}
bool TestFrameUploadEnd(void* texture) { return SUCCEEDED(HookTextureUnlock(texture,0)); }
bool TestSurfaceUnlock(void* surface)
{
    DDSURFACEDESC2 d{};void* native=nullptr;
    return GameDescription(surface,d,native) && native && SUCCEEDED(HookUnlock(native,nullptr));
}
void TestGameFill(void *gameSurface, const int *clip, const int *rect, DWORD color, TestFill original)
{
    realGameFill = original;
    HookGameFill(gameSurface, nullptr, clip, rect, color);
}
bool TestGameCopy(void *dest, const int *dr, void *source, const int *sr, void *copier, TestCopy original)
{
    realGameCopy = original;
    return HookGameCopy(dest, dr, source, sr, copier, 0, 3, 1000, 0);
}
} // namespace vt::Presentation32
