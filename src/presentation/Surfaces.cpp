// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "State.h"

// DirectDraw/BSurface lifetime, native copy/fill and sparse sidecar propagation.
namespace vt::Presentation32::detail
{
using Desc = HRESULT(WINAPI *)(void *, DDSURFACEDESC2 *);
bool Describe(void *object, DDSURFACEDESC2 &d)
{
    if (!object)
        return false;
    d = {};
    d.dwSize = sizeof(d);
    return SUCCEEDED(((Desc)Original(object, 22))(object, &d)) && d.lpSurface && d.dwWidth && d.dwHeight &&
           d.dwWidth <= 8192 && d.dwHeight <= 8192 && d.lPitch >= (LONG)d.dwWidth * 2 &&
           d.ddpfPixelFormat.dwRGBBitCount == 16 && d.ddpfPixelFormat.dwRBitMask == 0xF800 &&
           d.ddpfPixelFormat.dwGBitMask == 0x7E0 && d.ddpfPixelFormat.dwBBitMask == 0x1F;
}
std::shared_ptr<Buffer> Remember(void *object, const DDSURFACEDESC2 &d)
{
    // Call only with our lock held; never call a native COM method here.
    auto &b = state->buffers[d.lpSurface];
    if (!b || b->width != (int)d.dwWidth || b->height != (int)d.dwHeight || b->pitch != d.lPitch / 2)
        b = std::make_shared<Buffer>(d, state->options);
    auto &surface = state->surfaces[object];
    surface.desc = d;
    surface.buffer = b;
    if (d.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)
        state->primary = object;
    state->stats.surfaces = (LONG)state->surfaces.size();
    return b;
}
PixelRect Rect(const RECT *r, const DDSURFACEDESC2 &d)
{
    return r ? PixelRect{r->left, r->top, r->right, r->bottom} : PixelRect{0, 0, (int)d.dwWidth, (int)d.dwHeight};
}
bool InBounds(PixelRect r, const DDSURFACEDESC2 &d)
{
    return r.left >= 0 && r.top >= 0 && r.right > r.left && r.bottom > r.top && r.right <= (int)d.dwWidth &&
           r.bottom <= (int)d.dwHeight;
}
// XSurface::Blit's raster-copy helper uses ECX=dest, EDX=dest XYWH
// and seven stack arguments. It is also used by software DSurface blits.
GameCopy realGameCopy = nullptr;
thread_local int gameCopyDepth = 0;
struct CopyPublication
{
    Buffer* buffer;
    PixelRect rect;
    bool finalized=false;
    CopyPublication* prior;
};
thread_local CopyPublication* pendingCopy=nullptr;
void FinalizeCopy(CopyPublication& copy)
{
    if(!copy.buffer || copy.finalized) return;
    copy.buffer->plane.ResolveSubtitleNative(copy.buffer->base,copy.buffer->pitch,copy.rect);
    copy.finalized=true;
}
bool GameDescription(void *object, DDSURFACEDESC2 &d, void *&native)
{
    if (!object)
        return false;
    // DSurface's type-query slot is shared by every native DSurface.
    // Other XSurfaces own their buffer directly at +14; do not interpret
    // their allocator at +1C as an IDirectDrawSurface pointer.
    if ((uintptr_t)(*(void ***)object)[33] == 0x4C1AB0)
    {
        native = *(void **)((char *)object + 0x1C);
        return Describe(native, d);
    }
    native = nullptr;
    const int *fields = (const int *)object;
    const int width = fields[1], height = fields[2], bytes = fields[4];
    if (width <= 0 || width > 8192 || height <= 0 || height > 8192 || bytes != 2 || !fields[5])
        return false;
    using Pitch = int(__thiscall *)(void *);
    const int pitch = ((Pitch)(*(void ***)object)[29])(object);
    if (pitch < width * 2 || pitch % 2)
        return false;
    d = {};
    d.dwSize = sizeof(d);
    d.dwWidth = width;
    d.dwHeight = height;
    d.lPitch = pitch;
    d.lpSurface = *(void **)((char *)object + 0x14);
    return true;
}
void ForgetCpuText(void *object)
{
    Guard guard;
    auto found = state->cpuTextSurfaces.find(object);
    if (found == state->cpuTextSurfaces.end())
        return;
    const auto buffer = state->buffers.find(found->second->base);
    if (buffer != state->buffers.end() && buffer->second == found->second)
        state->buffers.erase(buffer);
    state->cpuTextSurfaces.erase(found);
}
CpuDelete realCpuDelete = nullptr;
void *__fastcall HookCpuDelete(void *object, void *, unsigned int flags)
{
    // Drop our sidecar while the native allocation is still alive. Never
    // touch its buffer after the scalar deleting destructor has freed it.
    ForgetCpuText(object);
    return realCpuDelete(object, flags);
}
void TrackCpuText(void *object)
{
    DDSURFACEDESC2 d{};
    void *native = nullptr;
    if (!GameDescription(object, d, native) || native)
        return;
    Guard guard;
    auto found = state->cpuTextSurfaces.find(object);
    std::shared_ptr<Buffer> b = found != state->cpuTextSurfaces.end() ? found->second : nullptr;
    if (!b || b->base != d.lpSurface || b->width != (int)d.dwWidth || b->height != (int)d.dwHeight ||
        b->pitch != d.lPitch / 2)
    {
        if (b)
            ForgetCpuText(object);
        b = std::make_shared<Buffer>(d, state->options);
        b->cpuText = true;
        state->cpuTextSurfaces[object] = b;
        state->buffers[d.lpSurface] = b;
        Log::Note("Present32: CPU text surface registered object=%p base=%p size=%ux%u pitch=%ld hi-raster=%d", object,
                  d.lpSurface, d.dwWidth, d.dwHeight, d.lPitch / 2, state->options.highResolution);
    }
    b->plane.ValidateNative(b->base, b->pitch);
    // BSurface::Lock has no DirectDraw Lock hook. Each BitText call is a
    // new paint boundary, so identical redraws replace old edge coverage.
    b->plane.BeginWrite();
}
bool GameRects(const int *dr, const DDSURFACEDESC2 &dd, const int *sr, const DDSURFACEDESC2 &sd, PixelRect &dest,
               PixelRect &source)
{
    if (!dr || !sr || dr[2] <= 0 || dr[3] <= 0 || sr[2] <= 0 || sr[3] <= 0)
        return false;
    // Match native ClipBlit (0x7BBE20): equal-sized rectangles clip as
    // a pair, preserving their offset. Different sizes clip separately.
    if (dr[2] == sr[2] && dr[3] == sr[3])
    {
        const int64_t l = std::max<int64_t>({0, -(int64_t)dr[0], -(int64_t)sr[0]});
        const int64_t t = std::max<int64_t>({0, -(int64_t)dr[1], -(int64_t)sr[1]});
        const int64_t r = std::min<int64_t>({dr[2], (int64_t)dd.dwWidth - dr[0], (int64_t)sd.dwWidth - sr[0]});
        const int64_t b = std::min<int64_t>({dr[3], (int64_t)dd.dwHeight - dr[1], (int64_t)sd.dwHeight - sr[1]});
        if (r <= l || b <= t)
            return false;
        dest = {(int)(dr[0] + l), (int)(dr[1] + t), (int)(dr[0] + r), (int)(dr[1] + b)};
        source = {(int)(sr[0] + l), (int)(sr[1] + t), (int)(sr[0] + r), (int)(sr[1] + b)};
    }
    else
    {
        auto clipped = [](const int *r, const DDSURFACEDESC2 &d)
        {
            return PixelRect{(int)std::max<int64_t>(0, r[0]), (int)std::max<int64_t>(0, r[1]),
                             (int)std::min<int64_t>(d.dwWidth, (int64_t)r[0] + r[2]),
                             (int)std::min<int64_t>(d.dwHeight, (int64_t)r[1] + r[3])};
        };
        dest = clipped(dr, dd);
        source = clipped(sr, sd);
    }
    return InBounds(dest, dd) && InBounds(source, sd);
}
bool __fastcall HookGameCopy(void *dest, const int *dr, void *source, const int *sr, void *copier, int remap, int mode,
                             int amount, int extra)
{
    DDSURFACEDESC2 dd{}, sd{};
    void *dn = nullptr;
    void *sn = nullptr;
    std::shared_ptr<Buffer> db;
    std::unique_ptr<PixelPlane> saved;
    PixelRect d{}, s{};
    const uintptr_t operation = copier ? *(uintptr_t *)copier : 0;
    if (state->enabled && (operation == 0x7F7BC4 || operation == 0x7F7BF4) && !remap && mode == 3 && amount == 1000 &&
        !extra && GameDescription(dest, dd, dn) && dn && GameDescription(source, sd, sn) &&
        GameRects(dr, dd, sr, sd, d, s))
    {
        Guard guard;
        db = Remember(dn, dd);
        auto found = state->buffers.find(sd.lpSurface);
        std::shared_ptr<Buffer> sb = sn ? Remember(sn, sd) : found != state->buffers.end() ? found->second : nullptr;
        db->plane.ValidateNative(db->base, db->pitch);
        if (sb)
            sb->plane.ValidateNative(sb->base, sb->pitch);
        if (db->plane.Intersects(d) || (sb && sb->plane.Intersects(s)))
        {
            const auto copyStart = Stamp();
            saved = std::make_unique<PixelPlane>(db->plane);
            db->plane.Copy(sb == db ? saved.get()
                           : sb     ? &sb->plane
                                    : nullptr,
                           s, d, (unsigned short *)sd.lpSurface, sd.lPitch / 2, db->base, db->pitch,
                           operation == 0x7F7BF4, 0, 0);
            state->stats.copyMs += Elapsed(copyStart);
            ++state->stats.copies;
            static LONG logged = 0;
            if (InterlockedIncrement(&logged) <= 3)
                Log::Note("Present32: native CPU copy text sync dest=(%d,%d,%d,%d) keyed=%d", d.left, d.top,
                          d.right - d.left, d.bottom - d.top, operation == 0x7F7BF4);
        }
    }
    // The copy locks DDS buffers internally before moving compatibility
    // pixels. Do not validate the new mask against pre-copy destination
    // bytes: this operation already synchronizes its region.
    if (saved)
        ++gameCopyDepth;
    CopyPublication publication{saved ? db.get() : nullptr,d,false,pendingCopy};
    pendingCopy=&publication;
    const bool ok = realGameCopy(dest, dr, source, sr, copier, remap, mode, amount, extra);
    pendingCopy=publication.prior;
    if (saved)
        --gameCopyDepth;
    if (!ok && saved)
    {
        Guard guard;
        db->plane = *saved;
    }
    if (ok && saved) {
        Guard guard;
        // CPU-only helpers may not call DDS Unlock; retain the post-copy
        // fallback. Normal game copies finalized before waking the renderer.
        FinalizeCopy(publication);
    }
    return ok;
}
GameFill realGameFill = nullptr;
bool __fastcall HookGameFill(void *object, void *, const int *clip, const int *rect, DWORD color)
{
    // Verified in gamemd.exe 1.001: DSurface+1C is IDirectDrawSurface*,
    // RectangleStruct is X/Y/W/H. FillRect/Fill route through FillRectEx.
    DDSURFACEDESC2 d{};
    std::shared_ptr<Buffer> buffer;
    std::unique_ptr<PixelPlane> saved;
    void *native = *(void **)((char *)object + 0x1C);
    if (rect && clip && Describe(native, d))
    {
        const int64_t l = std::max<int64_t>({0, rect[0], clip[0]});
        const int64_t t = std::max<int64_t>({0, rect[1], clip[1]});
        const int64_t r = std::min<int64_t>({d.dwWidth, (int64_t)rect[0] + rect[2], (int64_t)clip[0] + clip[2]});
        const int64_t b = std::min<int64_t>({d.dwHeight, (int64_t)rect[1] + rect[3], (int64_t)clip[1] + clip[3]});
        if (r > l && b > t && rect[2] > 0 && rect[3] > 0 && clip[2] > 0 && clip[3] > 0)
        {
            Guard guard;
            buffer = Remember(native, d);
            const PixelRect region{(int)l, (int)t, (int)r, (int)b};
            if (buffer->plane.Intersects(region))
            {
                saved = std::make_unique<PixelPlane>(buffer->plane);
                buffer->plane.Clear(region);
            }
        }
    }
    const bool ok = realGameFill(object, clip, rect, color);
    if (!ok && buffer && saved)
    {
        Guard guard;
        buffer->plane = *saved;
    }
    return ok;
}
void TrackSurface(void *object);
using Blt = HRESULT(WINAPI *)(void *, RECT *, void *, RECT *, DWORD, DDBLTFX *);
HRESULT WINAPI HookBlt(void *dest, RECT *dr, void *source, RECT *sr, DWORD flags, DDBLTFX *fx)
{
    auto original = (Blt)Original(dest, 5);
    DDSURFACEDESC2 dd{}, sd{};
    std::shared_ptr<Buffer> db;
    std::unique_ptr<PixelPlane> saved;
    PixelRect copied{};
    if (Describe(dest, dd) && (!source || Describe(source, sd)))
    {
        PixelRect d = Rect(dr, dd), s = Rect(sr, sd);
        copied=d;
        // Unsupported raster ops do not enter the high-precision copy path.
        const DWORD supported = DDBLT_WAIT | DDBLT_ASYNC | DDBLT_COLORFILL | DDBLT_KEYSRC | DDBLT_KEYDEST |
                                DDBLT_KEYSRCOVERRIDE | DDBLT_KEYDESTOVERRIDE | DDBLT_DDFX;
        if (InBounds(d, dd) && (!source || InBounds(s, sd)))
        {
            Guard guard;
            db = Remember(dest, dd);
            db->plane.ValidateNative(db->base, db->pitch);
            saved = std::make_unique<PixelPlane>(db->plane);
            if ((flags & DDBLT_COLORFILL) || (flags & ~supported) || !source)
                db->plane.Clear(d);
            else
            {
                auto sb = Remember(source, sd);
                sb->plane.ValidateNative(sb->base, sb->pitch);
                DDCOLORKEY sk = sd.ddckCKSrcBlt, dk = dd.ddckCKDestBlt;
                if (fx && (flags & DDBLT_KEYSRCOVERRIDE))
                    sk = fx->ddckSrcColorkey;
                if (fx && (flags & DDBLT_KEYDESTOVERRIDE))
                    dk = fx->ddckDestColorkey;
                db->plane.Copy(sb == db ? saved.get() : &sb->plane, s, d, sb->base, sb->pitch, db->base, db->pitch,
                               (flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) != 0,
                               (unsigned short)sk.dwColorSpaceLowValue, (unsigned short)sk.dwColorSpaceHighValue,
                               (flags & (DDBLT_KEYDEST | DDBLT_KEYDESTOVERRIDE)) != 0,
                               (unsigned short)dk.dwColorSpaceLowValue, (unsigned short)dk.dwColorSpaceHighValue,
                               fx && (flags & DDBLT_DDFX) && (fx->dwDDFX & DDBLTFX_MIRRORLEFTRIGHT),
                               fx && (flags & DDBLT_DDFX) && (fx->dwDDFX & DDBLTFX_MIRRORUPDOWN));
            }
        }
    }
    // cnc-ddraw may wake its renderer here: never hold our lock while
    // entering its surface/global critical sections.
    const HRESULT hr = original(dest, dr, source, sr, flags, fx);
    if (FAILED(hr) && db && saved)
    {
        Guard guard;
        db->plane = *saved;
    }
    if (SUCCEEDED(hr) && db && saved) {
        Guard guard;
        db->plane.ResolveSubtitleNative(db->base,db->pitch,copied);
    }
    return hr;
}
using BltFast = HRESULT(WINAPI *)(void *, DWORD, DWORD, void *, RECT *, DWORD);
HRESULT WINAPI HookBltFast(void *dest, DWORD x, DWORD y, void *source, RECT *sr, DWORD flags)
{
    DDSURFACEDESC2 dd{}, sd{};
    std::shared_ptr<Buffer> db;
    std::unique_ptr<PixelPlane> saved;
    PixelRect copied{};
    if (Describe(dest, dd) && Describe(source, sd))
    {
        PixelRect s = Rect(sr, sd), d{(int)x, (int)y, (int)x + s.right - s.left, (int)y + s.bottom - s.top};
        copied=d;
        if (InBounds(s, sd) && InBounds(d, dd))
        {
            Guard guard;
            db = Remember(dest, dd);
            auto sb = Remember(source, sd);
            db->plane.ValidateNative(db->base, db->pitch);
            sb->plane.ValidateNative(sb->base, sb->pitch);
            saved = std::make_unique<PixelPlane>(db->plane);
            auto sk = sd.ddckCKSrcBlt, dk = dd.ddckCKDestBlt;
            db->plane.Copy(sb == db ? saved.get() : &sb->plane, s, d, sb->base, sb->pitch, db->base, db->pitch,
                           (flags & DDBLTFAST_SRCCOLORKEY) != 0, (unsigned short)sk.dwColorSpaceLowValue,
                           (unsigned short)sk.dwColorSpaceHighValue, (flags & DDBLTFAST_DESTCOLORKEY) != 0,
                           (unsigned short)dk.dwColorSpaceLowValue, (unsigned short)dk.dwColorSpaceHighValue);
        }
    }
    auto original = (BltFast)Original(dest, 7);
    HRESULT hr = original(dest, x, y, source, sr, flags);
    if (FAILED(hr) && db && saved)
    {
        Guard guard;
        db->plane = *saved;
    }
    if (SUCCEEDED(hr) && db && saved) {
        Guard guard;
        db->plane.ResolveSubtitleNative(db->base,db->pitch,copied);
    }
    return hr;
}
using Lock = HRESULT(WINAPI *)(void *, RECT *, DDSURFACEDESC2 *, DWORD, HANDLE);
HRESULT WINAPI HookLock(void *object, RECT *rect, DDSURFACEDESC2 *output, DWORD flags, HANDLE event)
{
    HRESULT hr = ((Lock)Original(object, 25))(object, rect, output, flags, event);
    DDSURFACEDESC2 d{};
    if (SUCCEEDED(hr) && Describe(object, d))
    {
        Guard guard;
        auto b = Remember(object, d);
        if (!gameCopyDepth)
        {
            b->plane.ValidateNative(b->base, b->pitch);
            if (!(flags & DDLOCK_READONLY))
                b->plane.BeginWrite();
        }
    }
    return hr;
}
using Unlock = HRESULT(WINAPI *)(void *, void *);
HRESULT WINAPI HookUnlock(void *object, void *rectOrAddress)
{
    {
        Guard guard;
        auto found = state->surfaces.find(object);
        if(gameCopyDepth && found!=state->surfaces.end() && found->second.buffer) {
            for(auto* copy=pendingCopy;copy;copy=copy->prior)
                if(copy->buffer==found->second.buffer.get()) {
                    // cnc-ddraw Unlock wakes its renderer (and can delay its
                    // return for frame limiting). Finish native markers while
                    // pixels and metadata still belong to the same frame.
                    FinalizeCopy(*copy);break;
                }
        }
        if (!gameCopyDepth && found != state->surfaces.end() && found->second.buffer)
        {
            auto &s = found->second;
            s.buffer->plane.ValidateNative(s.buffer->base, s.buffer->pitch);
        }
    }
    return ((Unlock)Original(object, 32))(object, rectOrAddress);
}
using ReleaseDC = HRESULT(WINAPI *)(void *, HDC);
HRESULT WINAPI HookReleaseDC(void *object, HDC dc)
{
    DDSURFACEDESC2 d{};
    if (Describe(object, d))
    {
        Guard guard;
        Remember(object, d)->plane.Clear(Rect(nullptr, d));
    }
    return ((ReleaseDC)Original(object, 26))(object, dc);
}
using Attached = HRESULT(WINAPI *)(void *, void *, void **);
HRESULT WINAPI HookAttached(void *object, void *caps, void **output)
{
    HRESULT hr = ((Attached)Original(object, 12))(object, caps, output);
    if (SUCCEEDED(hr) && output)
        TrackSurface(*output);
    return hr;
}
using Query = HRESULT(WINAPI *)(void *, REFIID, void **);
HRESULT WINAPI HookSurfaceQuery(void *object, REFIID iid, void **output)
{
    HRESULT hr = ((Query)Original(object, 0))(object, iid, output);
    // cnc-ddraw exposes surface 1..7 with the same first 36 methods.
    if (SUCCEEDED(hr) && output && *output)
        TrackSurface(*output);
    return hr;
}
Release realSurfaceRelease = nullptr;
ULONG WINAPI HookSurfaceRelease(void *object)
{
    // Hook the implementation as well as virtual callers: releasing a
    // primary recursively releases attached buffers inside cnc-ddraw.
    ULONG refs = realSurfaceRelease(object);
    if (!refs)
    {
        Guard guard;
        state->surfaces.erase(object);
        if (state->primary == object)
            state->primary = nullptr;
        // Flip swaps storage addresses. Reclaim only buffers with no
        // remaining owners; don't confuse a COM pointer with its pixels.
        for (auto it = state->buffers.begin(); it != state->buffers.end();)
        {
            if (it->second.use_count() == 1)
                it = state->buffers.erase(it);
            else
                ++it;
        }
        state->stats.surfaces = (LONG)state->surfaces.size();
    }
    return refs;
}
void TrackSurface(void *object)
{
    if (!object)
        return;
    if (!realSurfaceRelease)
    {
        void *target = (*(void ***)object)[2];
        if (MH_CreateHook(target, (void *)HookSurfaceRelease, (void **)&realSurfaceRelease) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            Disable("surface lifetime detour failed");
            return;
        }
    }
    Patch(object, 0, (void *)HookSurfaceQuery);
    Patch(object, 5, (void *)HookBlt);
    Patch(object, 7, (void *)HookBltFast);
    Patch(object, 12, (void *)HookAttached);
    Patch(object, 25, (void *)HookLock);
    Patch(object, 26, (void *)HookReleaseDC);
    Patch(object, 32, (void *)HookUnlock);
    DDSURFACEDESC2 d{};
    if (Describe(object, d))
    {
        Guard guard;
        Remember(object, d);
    }
}
using CreateSurface = HRESULT(WINAPI *)(void *, void *, void **, IUnknown *);
HRESULT WINAPI HookCreateSurface(void *object, void *desc, void **output, IUnknown *outer)
{
    HRESULT hr = ((CreateSurface)Original(object, 6))(object, desc, output, outer);
    if (SUCCEEDED(hr) && output)
        TrackSurface(*output);
    return hr;
}
HRESULT WINAPI HookDDQuery(void *object, REFIID iid, void **output)
{
    HRESULT hr = ((Query)Original(object, 0))(object, iid, output);
    if (SUCCEEDED(hr) && output && *output)
    {
        Patch(*output, 0, (void *)HookDDQuery);
        Patch(*output, 6, (void *)HookCreateSurface);
    }
    return hr;
}
} // namespace vt::Presentation32::detail
