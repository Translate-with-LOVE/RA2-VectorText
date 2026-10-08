// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Atlas.h"

// D3D9 texture promotion, uploads and GPU overlay; restore caller device state.
namespace vt::Presentation32::detail
{
// D3D9: promote cnc-ddraw's RGB565 upload textures to A8R8G8B8 while keeping
// its staging rectangle 16-bit. Ordinary output composites text during upload;
// HiDPI uploads the world separately, then draws the 2x text atlas at the actual
// viewport scale. Neither presentation path quantizes the text back to RGB565.
using Create9 = IDirect3D9 *(WINAPI *)(UINT);
Create9 realCreate9 = nullptr;
using Device = HRESULT(WINAPI *)(void *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, void **);
using TextureLock = HRESULT(WINAPI *)(void *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
using TextureUnlock = HRESULT(WINAPI *)(void *, UINT);
using Primitive = HRESULT(WINAPI *)(void *, D3DPRIMITIVETYPE, UINT, UINT);
using Reset = HRESULT(WINAPI *)(void *, D3DPRESENT_PARAMETERS *);
bool ScreenQuad(IDirect3DDevice9 *device, UINT start, ScreenVertex (&vertices)[4])
{
    DWORD fvf = 0;
    IDirect3DVertexBuffer9 *buffer = nullptr;
    UINT offset = 0, stride = 0;
    if (FAILED(device->GetFVF(&fvf)) || fvf != (D3DFVF_XYZRHW | D3DFVF_TEX1) ||
        FAILED(device->GetStreamSource(0, &buffer, &offset, &stride)) || !buffer)
        return false;
    void *data = nullptr;
    const bool ok = stride == sizeof(ScreenVertex) &&
                    SUCCEEDED(buffer->Lock(offset + start * stride, sizeof(vertices), &data, D3DLOCK_READONLY));
    if (ok)
    {
        memcpy(vertices, data, sizeof(vertices));
        buffer->Unlock();
    }
    buffer->Release();
    return ok;
}

// The game quad reflects cnc-ddraw's effective configuration, including
// borderless/exclusive fullscreen, integer boxing, and viewport offsets.
HRESULT WINAPI HookPrimitive(void *object, D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    const auto original = (Primitive)Original(object, 81);
    if (!state->autoTextScale || !FromCnc(_ReturnAddress()) || type != D3DPT_TRIANGLESTRIP || count != 2)
        return original(object, type, start, count);
    auto *device = (IDirect3DDevice9 *)object;
    IDirect3DBaseTexture9 *texture = nullptr;
    ScreenVertex vertices[4]{};
    if (FAILED(device->GetTexture(0, &texture)) || !texture)
        return original(object, type, start, count);
    const bool quad = ScreenQuad(device, start, vertices);
    auto primary = Primary();
    bool overlay = false;
    IDirect3DStateBlock9 *saved = nullptr;
    {
        Guard guard;
        auto it = state->textures.find(texture);
        if (quad && primary && it != state->textures.end())
        {
            auto &t = it->second;
            float sx = 0, sy = 0;
            const bool scaled = OutputScale(t, vertices, sx, sy);
            const bool doubled = fabs(sx - t.rasterScale) < 0.0001f && fabs(sy - t.rasterScale) < 0.0001f;
            if(scaled) SetOutputRasterScale((int)ceil(std::min(8.0f,std::max(sx,sy))-0.0001f));
            if (scaled && !t.overlayAttempted)
            {
                t.overlayAttempted = true;
                t.device = device;
                if (ResizeAtlas(t, primary->plane.TileCount()))
                {
                    t.overlayReady = true;
                }
                if (!t.overlay)
                    Log::Note("Present32: output text scale=1 (text atlas unavailable)");
            }
            if (scaled && t.overlay && (fabs(t.reportedScale - sx) > 0.0001f || fabs(t.reportedScaleY - sy) > 0.0001f))
            {
                t.reportedScale = sx;
                t.reportedScaleY = sy;
                Log::Note("Present32: output text scale=%.4gx%.4g viewport=%.0fx%.0f logical=%dx%d requested-raster=%d atlas-raster=%d filter=%s",
                          sx, sy, vertices[2].x - vertices[0].x, vertices[0].y - vertices[1].y, primary->width,
                          primary->height, OutputRasterScale(),t.rasterScale,doubled ? "point" : "linear");
            }
            if (!scaled)
                t.reportedScale = t.reportedScaleY = 0;
            // Only frames whose world upload excluded its old text get an
            // overlay. First frame and any failure keep the 1x composition.
            const auto overlayStart = Stamp();
            overlay = t.cleanWorld && t.overlay && !t.overlayTiles.empty() &&
                      SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &saved));
            // Readiness describes the observed output, not whether an atlas
            // is currently allocated. UploadOverlay can recreate an atlas
            // after a density change or retry a transient allocation failure.
            t.overlayReady = scaled && t.device;
            if (overlay)
            {
                const HRESULT hr = original(object, type, start, count);
                device->SetPixelShader(nullptr);
                device->SetVertexShader(nullptr);
                device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
                device->SetTexture(0, t.overlay);
                device->SetTexture(1, nullptr);
                device->SetRenderState(D3DRS_ZENABLE, FALSE);
                device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
                device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
                device->SetRenderState(D3DRS_SRGBWRITEENABLE, state->options.linear);
                device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, state->options.linear);
                device->SetSamplerState(0, D3DSAMP_MAGFILTER, doubled ? D3DTEXF_POINT : D3DTEXF_LINEAR);
                device->SetSamplerState(0, D3DSAMP_MINFILTER, doubled ? D3DTEXF_POINT : D3DTEXF_LINEAR);
                device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                AtlasVertices(t, vertices, sx, sy);
                if (!t.overlayVertices.empty())
                    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)t.overlayVertices.size() / 3,
                                            t.overlayVertices.data(), sizeof(ScreenVertex));
                saved->Apply();
                saved->Release();
                state->stats.overlayMs += Elapsed(overlayStart);
                ++state->stats.overlays;
                texture->Release();
                return hr;
            }
        }
    }
    texture->Release();
    return original(object, type, start, count);
}

HRESULT WINAPI HookReset(void *object, D3DPRESENT_PARAMETERS *pp)
{
    const auto original = (Reset)Original(object, 16);
    D3DPRESENT_PARAMETERS converted{};
    if (pp && pp->BackBufferFormat == D3DFMT_R5G6B5 && state->enabled)
    {
        converted = *pp;
        converted.BackBufferFormat = D3DFMT_X8R8G8B8;
        const HRESULT hr = original(object, &converted);
        if (SUCCEEDED(hr))
            *pp = converted;
        return hr;
    }
    return original(object, pp);
}
HRESULT WINAPI HookTextureLock(void *object, UINT level, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags)
{
    auto original = (TextureLock)Original(object, 19);
    HRESULT hr = original(object, level, out, rect, flags);
    if (FAILED(hr) || level || !out)
        return hr;
    auto primary=Primary();
    Guard guard;
    auto it = state->textures.find(object);
    if (it == state->textures.end())
        return hr;
    auto &t = it->second;
    t.rect = rect ? *rect : RECT{0, 0, t.width, t.height};
    t.native = *out;
    int w = t.rect.right - t.rect.left, h = t.rect.bottom - t.rect.top;
    t.scratch.resize((size_t)w * h);
    // cnc-ddraw stages the native pixels between LockRect and UnlockRect.
    // The game can already be writing the next frame by the latter call.
    // Freeze its matching text metadata now; old uploads must never validate
    // (and erase) the game thread's newer live plane.
    t.framePlane.reset();
    if(primary && t.rect.left==0 && t.rect.top==0 && w==primary->width && h==primary->height)
        t.framePlane=std::make_unique<PixelPlane>(primary->plane);
    t.locked = true;
    out->pBits = t.scratch.data();
    out->Pitch = w * 2;
    return hr;
}
HRESULT WINAPI HookTextureUnlock(void *object, UINT level)
{
    auto original = (TextureUnlock)Original(object, 20);
    bool tracked;
    {
        Guard guard;
        tracked = !level && state->textures.find(object) != state->textures.end();
    }
    if (!tracked)
        return original(object, level);
    {
        Guard guard;
        auto it = state->textures.find(object);
        if (!level && it != state->textures.end() && it->second.locked)
        {
            auto &t = it->second;
            const int w = t.rect.right - t.rect.left, h = t.rect.bottom - t.rect.top;
            auto *output = (uint32_t *)t.native.pBits;
            // Native updates use a full game-sized rectangle beginning at
            // zero; unsupported partial updates still get valid 32-bit RGB.
            if (t.framePlane)
            {
                const auto uploadStart = Stamp();
                auto& frame=*t.framePlane;
                frame.ValidateNative(t.scratch.data(), w);
                t.cleanWorld = t.overlayReady && UploadOverlay(t, frame);
                if (t.cleanWorld)
                    frame.BackgroundRect(t.scratch.data(), w, output, t.native.Pitch / 4, {0, 0, w, h});
                else
                    frame.CompositeRect(t.scratch.data(), w, output, t.native.Pitch / 4, {0, 0, w, h});
                state->stats.uploadMs += Elapsed(uploadStart);
                ++state->stats.uploads;
                RecordFrame(output, w, h, t.native.Pitch / 4);
            }
            else
            {
                t.cleanWorld = false;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        output[(size_t)y * (t.native.Pitch / 4) + x] =
                            PixelPlane::Expand565(t.scratch[(size_t)y * w + x]);
            }
            t.locked = false;
            t.framePlane.reset();
        }
    }
    return original(object, level);
}
ULONG WINAPI HookTextureRelease(void *object)
{
    auto original = (Release)Original(object, 2);
    ULONG count = original(object);
    if (!count)
    {
        Guard guard;
        auto it = state->textures.find(object);
        if (it != state->textures.end() && it->second.overlay)
            it->second.overlay->Release();
        state->textures.erase(object);
    }
    return count;
}
HRESULT WINAPI HookCreateTexture(void *object, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                 void **out, HANDLE *shared)
{
    auto original = (CreateTexture)Original(object, 23);
    if (state->enabled && FromCnc(_ReturnAddress()) && format == D3DFMT_R5G6B5 && levels == 1 && !shared)
    {
        HRESULT hr = original(object, w, h, levels, usage, D3DFMT_A8R8G8B8, pool, out, shared);
        if (SUCCEEDED(hr) && out && *out && Patch(*out, 19, (void *)HookTextureLock) &&
            Patch(*out, 20, (void *)HookTextureUnlock) && Patch(*out, 2, (void *)HookTextureRelease))
        {
            Guard guard;
            state->textures.emplace(*out, Texture{(int)w, (int)h});
            state->stats.backend = 2;
            Log::Note("Present32: D3D9 upload texture %ux%u promoted to BGRA8", w, h);
            return hr;
        }
        if (SUCCEEDED(hr) && out && *out)
        {
            (*(IDirect3DTexture9 **)out)->Release();
            *out = nullptr;
        }
        Disable("D3D9 texture promotion failed");
    }
    return original(object, w, h, levels, usage, format, pool, out, shared);
}
HRESULT WINAPI HookCreateDevice(void *object, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                D3DPRESENT_PARAMETERS *pp, void **out)
{
    auto original = (Device)Original(object, 16);
    const bool cnc = FromCnc(_ReturnAddress());
    D3DPRESENT_PARAMETERS parameters{};
    D3DPRESENT_PARAMETERS *actual = pp;
    if (cnc && pp && pp->BackBufferFormat == D3DFMT_R5G6B5)
    {
        parameters = *pp;
        parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
        actual = &parameters;
    }
    // State getters are unavailable on pure devices. Needed only for the
    // opt-in automatic overlay, to preserve cnc-ddraw's quad and all state.
    if (cnc && state->autoTextScale)
        flags &= ~D3DCREATE_PUREDEVICE;
    HRESULT hr = original(object, adapter, type, window, flags, actual, out);
    if (FAILED(hr) && actual != pp)
    {
        Disable("D3D9 32-bit backbuffer unavailable");
        actual = pp;
        hr = original(object, adapter, type, window, flags, pp, out);
    }
    if (SUCCEEDED(hr) && actual != pp)
        *pp = parameters;
    if (SUCCEEDED(hr) && out && cnc)
    {
        Patch(*out, 23, (void *)HookCreateTexture);
        Patch(*out, 16, (void *)HookReset);
        if (state->autoTextScale && state->options.linear)
        {
            D3DCAPS9 caps{};
            auto *api = (IDirect3D9 *)object;
            auto *device = (IDirect3DDevice9 *)*out;
            // Linear premultiplied ink needs sRGB conversion after blending.
            // Older adapters retain the already verified 1x compositor.
            HRESULT capResult = device->GetDeviceCaps(&caps);
            HRESULT readResult = api->CheckDeviceFormat(adapter, type, actual->BackBufferFormat,
                                                        D3DUSAGE_QUERY_SRGBREAD, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
            HRESULT writeResult = api->CheckDeviceFormat(adapter, type, actual->BackBufferFormat,
                                                         D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_SRGBWRITE,
                                                         D3DRTYPE_TEXTURE, actual->BackBufferFormat);
            if (FAILED(capResult) || !(caps.PrimitiveMiscCaps & D3DPMISCCAPS_POSTBLENDSRGBCONVERT) ||
                FAILED(readResult) || FAILED(writeResult))
            {
                state->autoTextScale = false;
                Log::Note("Present32: output text scale=1 (linear overlay caps=%08lX read=%08lX write=%08lX)",
                          caps.PrimitiveMiscCaps, readResult, writeResult);
            }
        }
        if (state->autoTextScale && !Patch(*out, 81, (void *)HookPrimitive))
            state->autoTextScale = false;
    }
    return hr;
}
IDirect3D9 *WINAPI HookCreate9(UINT version)
{
    IDirect3D9 *object = realCreate9(version);
    if (object)
        Patch(object, 16, (void *)HookCreateDevice);
    return object;
}

FARPROC ResolveD3D9Create(FARPROC original)
{
    realCreate9 = (Create9)original;
    return (FARPROC)HookCreate9;
}
} // namespace vt::Presentation32::detail
