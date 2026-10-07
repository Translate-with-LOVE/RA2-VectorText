// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Presentation32.h"
#include "PixelPlane.h"
#include "PixelWriter.h"
#include "Logger.h"
#include "../third_party/minhook/include/MinHook.h"
#include <ddraw.h>
#include <d3d9.h>
#include <GL/gl.h>
#include <intrin.h>
#include <algorithm>
#include <map>
#include <memory>
#include <vector>
#include <cstring>
#include <cmath>

namespace vt { namespace Presentation32 { namespace {
    using Proc = FARPROC (WINAPI*)(HMODULE, LPCSTR);
    using CreateDD = HRESULT (WINAPI*)(GUID*, void**, IUnknown*);
    Proc realProc = ::GetProcAddress, importedProc = ::GetProcAddress;
    CreateDD realCreate = nullptr;
    HMODULE cncModule = nullptr;
    uintptr_t cncEnd = 0;
    struct Slot { void** table; int index; void* original; };
    struct Buffer {
        unsigned short* base; int width, height, pitch;
        PixelPlane plane;
        bool cpuText = false;
        Buffer(const DDSURFACEDESC2& d, PlaneOptions options) :
            base((unsigned short*)d.lpSurface), width((int)d.dwWidth), height((int)d.dwHeight),
            pitch(d.lPitch / 2), plane(width, height, options) {}
    };
    struct Surface {
        DDSURFACEDESC2 desc{};
        std::shared_ptr<Buffer> buffer;
    };
    struct ScreenVertex { float x,y,z,rhw,u,v; };
    struct Texture {
        int width, height;
        D3DLOCKED_RECT native{};
        RECT rect{};
        std::vector<unsigned short> scratch;
        bool locked = false;
        bool cleanWorld = false;
        IDirect3DTexture9* overlay = nullptr;
        IDirect3DDevice9* device = nullptr;
        int atlasWidth = 0, atlasHeight = 0, atlasColumns = 16;
        std::vector<ScreenVertex> overlayVertices;
        std::vector<PixelRect> overlayTiles;
        bool overlayReady = false;
        bool overlayAttempted = false;
    };
    struct State {
        CRITICAL_SECTION cs;
        std::vector<Slot> slots;
        std::map<void*, Surface> surfaces;
        std::map<void*, std::shared_ptr<Buffer>> buffers;
        std::map<void*, std::shared_ptr<Buffer>> cpuTextSurfaces;
        std::map<void*, Texture> textures;
        PlaneOptions options;
        void* primary = nullptr;
        Statistics stats{};
        bool enabled = false;
        bool autoTextScale = false;
        bool cpuTextReady = false;
        bool profile = false;
        LARGE_INTEGER frequency{};
#ifdef VT_PRESENT_TEST
        std::vector<uint32_t> last;
        int lastWidth = 0, lastHeight = 0;
#endif
        State() { InitializeCriticalSection(&cs); QueryPerformanceFrequency(&frequency); }
    };
    State* state = nullptr; // process-lifetime; no STL destruction under loader lock
    struct Guard {
        Guard() { EnterCriticalSection(&state->cs); }
        ~Guard() { LeaveCriticalSection(&state->cs); }
    };
    LONGLONG Stamp() {
        LARGE_INTEGER tick{};if(state->profile) QueryPerformanceCounter(&tick);return tick.QuadPart;
    }
    double Elapsed(LONGLONG start) { return state->profile ? (Stamp()-start)*1000.0/state->frequency.QuadPart : 0; }
    bool FromCnc(void* caller) {
        return cncModule && (uintptr_t)caller >= (uintptr_t)cncModule && (uintptr_t)caller < cncEnd;
    }
    void* Original(void* object, int slot) {
        void** table = *(void***)object;
        Guard guard;
        for (const auto& s : state->slots)
            if (s.table == table && s.index == slot) return s.original;
        return table[slot];
    }
    bool Patch(void* object, int index, void* replacement) {
        if (!object) return false;
        void** table = *(void***)object;
        Guard guard;
        for (const auto& s : state->slots)
            if (s.table == table && s.index == index) return table[index] == replacement;
        DWORD protection;
        if (!VirtualProtect(table + index, sizeof(void*), PAGE_EXECUTE_READWRITE, &protection)) return false;
        state->slots.push_back({table, index, table[index]});
        InterlockedExchangePointer(table + index, replacement);
        DWORD ignored;
        VirtualProtect(table + index, sizeof(void*), protection, &ignored);
        return true;
    }
    using Desc = HRESULT (WINAPI*)(void*, DDSURFACEDESC2*);
    bool Describe(void* object, DDSURFACEDESC2& d) {
        if (!object) return false;
        d = {}; d.dwSize = sizeof(d);
        return SUCCEEDED(((Desc)Original(object, 22))(object, &d)) && d.lpSurface &&
            d.dwWidth && d.dwHeight && d.dwWidth <= 8192 && d.dwHeight <= 8192 &&
            d.lPitch >= (LONG)d.dwWidth * 2 && d.ddpfPixelFormat.dwRGBBitCount == 16 &&
            d.ddpfPixelFormat.dwRBitMask == 0xF800 &&
            d.ddpfPixelFormat.dwGBitMask == 0x7E0 && d.ddpfPixelFormat.dwBBitMask == 0x1F;
    }
    std::shared_ptr<Buffer> Remember(void* object, const DDSURFACEDESC2& d) {
        // Call only with our lock held; never call a native COM method here.
        auto& b = state->buffers[d.lpSurface];
        if (!b || b->width != (int)d.dwWidth || b->height != (int)d.dwHeight || b->pitch != d.lPitch / 2)
            b = std::make_shared<Buffer>(d, state->options);
        auto& surface = state->surfaces[object];
        surface.desc = d; surface.buffer = b;
        if (d.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) state->primary = object;
        state->stats.surfaces = (LONG)state->surfaces.size();
        return b;
    }
    PixelRect Rect(const RECT* r, const DDSURFACEDESC2& d) {
        return r ? PixelRect{r->left,r->top,r->right,r->bottom}
            : PixelRect{0,0,(int)d.dwWidth,(int)d.dwHeight};
    }
    bool InBounds(PixelRect r, const DDSURFACEDESC2& d) {
        return r.left >= 0 && r.top >= 0 && r.right > r.left && r.bottom > r.top &&
            r.right <= (int)d.dwWidth && r.bottom <= (int)d.dwHeight;
    }
    // XSurface::Blit's raster-copy helper uses ECX=dest, EDX=dest XYWH
    // and seven stack arguments. It is also used by software DSurface blits.
    using GameCopy = bool (__fastcall*)(void*,const int*,void*,const int*,void*,int,int,int,int);
    GameCopy realGameCopy = nullptr;
    thread_local int gameCopyDepth = 0;
    bool GameDescription(void* object,DDSURFACEDESC2& d,void*& native) {
        if (!object) return false;
        // DSurface's type-query slot is shared by every native DSurface.
        // Other XSurfaces own their buffer directly at +14; do not interpret
        // their allocator at +1C as an IDirectDrawSurface pointer.
        if ((uintptr_t)(*(void***)object)[33] == 0x4C1AB0) {
            native=*(void**)((char*)object+0x1C);
            return Describe(native,d);
        }
        native=nullptr;
        const int* fields=(const int*)object;
        const int width=fields[1],height=fields[2],bytes=fields[4];
        if (width<=0 || width>8192 || height<=0 || height>8192 || bytes!=2 || !fields[5]) return false;
        using Pitch = int (__thiscall*)(void*);
        const int pitch=((Pitch)(*(void***)object)[29])(object);
        if (pitch<width*2 || pitch%2) return false;
        d={}; d.dwSize=sizeof(d); d.dwWidth=width; d.dwHeight=height;
        d.lPitch=pitch; d.lpSurface=*(void**)((char*)object+0x14);
        return true;
    }
    void ForgetCpuText(void* object) {
        Guard guard;
        auto found=state->cpuTextSurfaces.find(object);
        if(found==state->cpuTextSurfaces.end()) return;
        const auto buffer=state->buffers.find(found->second->base);
        if(buffer!=state->buffers.end() && buffer->second==found->second)
            state->buffers.erase(buffer);
        state->cpuTextSurfaces.erase(found);
    }
    using CpuDelete = void* (__thiscall*)(void*,unsigned int);
    CpuDelete realCpuDelete = nullptr;
    void* __fastcall HookCpuDelete(void* object,void*,unsigned int flags) {
        // Drop our sidecar while the native allocation is still alive. Never
        // touch its buffer after the scalar deleting destructor has freed it.
        ForgetCpuText(object);
        return realCpuDelete(object,flags);
    }
    void TrackCpuText(void* object) {
        DDSURFACEDESC2 d{};void* native=nullptr;
        if(!GameDescription(object,d,native) || native) return;
        Guard guard;
        auto found=state->cpuTextSurfaces.find(object);
        std::shared_ptr<Buffer> b=found!=state->cpuTextSurfaces.end() ? found->second : nullptr;
        if(!b || b->base!=d.lpSurface || b->width!=(int)d.dwWidth ||
            b->height!=(int)d.dwHeight || b->pitch!=d.lPitch/2) {
            if(b) ForgetCpuText(object);
            b=std::make_shared<Buffer>(d,state->options);b->cpuText=true;
            state->cpuTextSurfaces[object]=b;state->buffers[d.lpSurface]=b;
            Log::Note("Present32: CPU text surface registered object=%p base=%p size=%ux%u pitch=%ld hi-raster=%d",
                object,d.lpSurface,d.dwWidth,d.dwHeight,d.lPitch/2,state->options.highResolution);
        }
        b->plane.ValidateNative(b->base,b->pitch);
        // BSurface::Lock has no DirectDraw Lock hook. Each BitText call is a
        // new paint boundary, so identical redraws replace old edge coverage.
        b->plane.BeginWrite();
    }
    bool GameRects(const int* dr,const DDSURFACEDESC2& dd,const int* sr,const DDSURFACEDESC2& sd,
                   PixelRect& dest,PixelRect& source) {
        if (!dr || !sr || dr[2]<=0 || dr[3]<=0 || sr[2]<=0 || sr[3]<=0) return false;
        // Match native ClipBlit (0x7BBE20): equal-sized rectangles clip as
        // a pair, preserving their offset. Different sizes clip separately.
        if (dr[2]==sr[2] && dr[3]==sr[3]) {
            const int64_t l=std::max<int64_t>({0,-(int64_t)dr[0],-(int64_t)sr[0]});
            const int64_t t=std::max<int64_t>({0,-(int64_t)dr[1],-(int64_t)sr[1]});
            const int64_t r=std::min<int64_t>({dr[2],(int64_t)dd.dwWidth-dr[0],(int64_t)sd.dwWidth-sr[0]});
            const int64_t b=std::min<int64_t>({dr[3],(int64_t)dd.dwHeight-dr[1],(int64_t)sd.dwHeight-sr[1]});
            if (r<=l || b<=t) return false;
            dest={(int)(dr[0]+l),(int)(dr[1]+t),(int)(dr[0]+r),(int)(dr[1]+b)};
            source={(int)(sr[0]+l),(int)(sr[1]+t),(int)(sr[0]+r),(int)(sr[1]+b)};
        } else {
            auto clipped=[](const int* r,const DDSURFACEDESC2& d) {
                return PixelRect{(int)std::max<int64_t>(0,r[0]),(int)std::max<int64_t>(0,r[1]),
                    (int)std::min<int64_t>(d.dwWidth,(int64_t)r[0]+r[2]),
                    (int)std::min<int64_t>(d.dwHeight,(int64_t)r[1]+r[3])};
            };
            dest=clipped(dr,dd); source=clipped(sr,sd);
        }
        return InBounds(dest,dd) && InBounds(source,sd);
    }
    bool __fastcall HookGameCopy(void* dest,const int* dr,void* source,const int* sr,
                                 void* copier,int remap,int mode,int amount,int extra) {
        DDSURFACEDESC2 dd{},sd{}; void* dn=nullptr; void* sn=nullptr;
        std::shared_ptr<Buffer> db;
        std::unique_ptr<PixelPlane> saved;
        PixelRect d{},s{};
        const uintptr_t operation=copier ? *(uintptr_t*)copier : 0;
        if (state->enabled && (operation==0x7F7BC4 || operation==0x7F7BF4) &&
            !remap && mode==3 && amount==1000 && !extra &&
            GameDescription(dest,dd,dn) && dn && GameDescription(source,sd,sn) &&
            GameRects(dr,dd,sr,sd,d,s)) {
            Guard guard;
            db=Remember(dn,dd);
            auto found=state->buffers.find(sd.lpSurface);
            std::shared_ptr<Buffer> sb=sn ? Remember(sn,sd) :
                found!=state->buffers.end() ? found->second : nullptr;
            db->plane.ValidateNative(db->base,db->pitch);
            if(sb) sb->plane.ValidateNative(sb->base,sb->pitch);
            if (db->plane.Intersects(d) || (sb && sb->plane.Intersects(s))) {
                const auto copyStart=Stamp();
                saved=std::make_unique<PixelPlane>(db->plane);
                db->plane.Copy(sb==db ? saved.get() : sb ? &sb->plane : nullptr,s,d,
                    (unsigned short*)sd.lpSurface,sd.lPitch/2,db->base,db->pitch,
                    operation==0x7F7BF4,0,0);
                state->stats.copyMs+=Elapsed(copyStart);++state->stats.copies;
                static LONG logged=0;
                if (InterlockedIncrement(&logged)<=3)
                    Log::Note("Present32: native CPU copy text sync dest=(%d,%d,%d,%d) keyed=%d",
                        d.left,d.top,d.right-d.left,d.bottom-d.top,operation==0x7F7BF4);
            }
        }
        // The copy locks DDS buffers internally before moving compatibility
        // pixels. Do not validate the new mask against pre-copy destination
        // bytes: this operation already synchronizes its region.
        if (saved) ++gameCopyDepth;
        const bool ok=realGameCopy(dest,dr,source,sr,copier,remap,mode,amount,extra);
        if (saved) --gameCopyDepth;
        if (!ok && saved) { Guard guard; db->plane=*saved; }
        return ok;
    }
    using GameFill = bool (__thiscall*)(void*,const int*,const int*,DWORD);
    GameFill realGameFill = nullptr;
    bool __fastcall HookGameFill(void* object,void*,const int* clip,const int* rect,DWORD color) {
        // Verified in gamemd.exe 1.001: DSurface+1C is IDirectDrawSurface*,
        // RectangleStruct is X/Y/W/H. FillRect/Fill route through FillRectEx.
        DDSURFACEDESC2 d{};
        std::shared_ptr<Buffer> buffer;
        std::unique_ptr<PixelPlane> saved;
        void* native=*(void**)((char*)object+0x1C);
        if (rect && clip && Describe(native,d)) {
            const int64_t l=std::max<int64_t>({0,rect[0],clip[0]});
            const int64_t t=std::max<int64_t>({0,rect[1],clip[1]});
            const int64_t r=std::min<int64_t>({d.dwWidth,(int64_t)rect[0]+rect[2],(int64_t)clip[0]+clip[2]});
            const int64_t b=std::min<int64_t>({d.dwHeight,(int64_t)rect[1]+rect[3],(int64_t)clip[1]+clip[3]});
            if (r>l && b>t && rect[2]>0 && rect[3]>0 && clip[2]>0 && clip[3]>0) {
                Guard guard;
                buffer=Remember(native,d);
                const PixelRect region{(int)l,(int)t,(int)r,(int)b};
                if(buffer->plane.Intersects(region)) {
                    saved=std::make_unique<PixelPlane>(buffer->plane);
                    buffer->plane.Clear(region);
                }
            }
        }
        const bool ok=realGameFill(object,clip,rect,color);
        if (!ok && buffer && saved) { Guard guard; buffer->plane=*saved; }
        return ok;
    }
    int Draw(const Target& t, const GlyphCell& cell, int x, int y, int rows,
             unsigned short color, bool aa) {
        if (!state || !state->enabled || !t.base) return -1;
        Guard guard;
        if (aa != state->options.antialias) return -1;
        const auto found = state->buffers.find(t.base);
        if (found == state->buffers.end() || found->second->pitch != t.pitch) return -1;
        // The cached loading image can be painted before the first GPU upload.
        // Its compatibility pixels remain available until a presenter is ready.
        if (!state->stats.backend && !found->second->cpuText) return -1;
        const auto start=Stamp();
        found->second->plane.Paint(cell, x, y, rows, color,
            {t.clipL,t.clipT,t.clipR + 1,t.clipB + 1},found->second->base,found->second->pitch);
        state->stats.glyphMs+=Elapsed(start);
        ++state->stats.glyphs;
        return 1;
    }
    void Disable(const char* reason) {
        Guard guard;
        // If a presenter fails after text was already retained, materialize
        // that text once before declining future glyphs. Never drop it.
        for(auto& entry : state->buffers) {
            auto& b=*entry.second;
            const auto pixels=b.plane.CaptureNative(b.base,b.pitch,{0,0,b.width,b.height});
            for(const auto& p : pixels) {
                uint32_t rgb=b.plane.Composite(p.x,p.y,p.base);
                b.base[(size_t)p.y*b.pitch+p.x]=(unsigned short)(
                    (((rgb>>19)&31)<<11)|(((rgb>>10)&63)<<5)|((rgb>>3)&31));
            }
            b.plane.Clear({0,0,b.width,b.height});
        }
        state->enabled=false; state->stats.backend=0; SetPresentationWriter(nullptr);
        Log::Note("Present32: %s; retaining RGB565",reason);
    }
    void TrackSurface(void* object);
    using Blt = HRESULT (WINAPI*)(void*, RECT*, void*, RECT*, DWORD, DDBLTFX*);
    HRESULT WINAPI HookBlt(void* dest, RECT* dr, void* source, RECT* sr, DWORD flags, DDBLTFX* fx) {
        auto original = (Blt)Original(dest, 5);
        DDSURFACEDESC2 dd{}, sd{};
        std::shared_ptr<Buffer> db;
        std::unique_ptr<PixelPlane> saved;
        if (Describe(dest, dd) && (!source || Describe(source, sd))) {
            PixelRect d = Rect(dr, dd), s = Rect(sr, sd);
            // Unsupported raster ops do not enter the high-precision copy path.
            const DWORD supported = DDBLT_WAIT | DDBLT_ASYNC | DDBLT_COLORFILL |
                DDBLT_KEYSRC | DDBLT_KEYDEST | DDBLT_KEYSRCOVERRIDE | DDBLT_KEYDESTOVERRIDE | DDBLT_DDFX;
            if (InBounds(d,dd) && (!source || InBounds(s,sd))) {
                Guard guard;
                db = Remember(dest, dd);
                db->plane.ValidateNative(db->base,db->pitch);
                saved = std::make_unique<PixelPlane>(db->plane);
                if ((flags & DDBLT_COLORFILL) || (flags & ~supported) || !source)
                    db->plane.Clear(d);
                else {
                    auto sb = Remember(source, sd);
                    sb->plane.ValidateNative(sb->base,sb->pitch);
                    DDCOLORKEY sk = sd.ddckCKSrcBlt, dk = dd.ddckCKDestBlt;
                    if (fx && (flags & DDBLT_KEYSRCOVERRIDE)) sk = fx->ddckSrcColorkey;
                    if (fx && (flags & DDBLT_KEYDESTOVERRIDE)) dk = fx->ddckDestColorkey;
                    db->plane.Copy(sb==db ? saved.get() : &sb->plane,s,d,sb->base,sb->pitch,db->base,db->pitch,
                        (flags & (DDBLT_KEYSRC|DDBLT_KEYSRCOVERRIDE)) != 0,
                        (unsigned short)sk.dwColorSpaceLowValue,(unsigned short)sk.dwColorSpaceHighValue,
                        (flags & (DDBLT_KEYDEST|DDBLT_KEYDESTOVERRIDE)) != 0,
                        (unsigned short)dk.dwColorSpaceLowValue,(unsigned short)dk.dwColorSpaceHighValue,
                        fx && (flags & DDBLT_DDFX) && (fx->dwDDFX & DDBLTFX_MIRRORLEFTRIGHT),
                        fx && (flags & DDBLT_DDFX) && (fx->dwDDFX & DDBLTFX_MIRRORUPDOWN));
                }
            }
        }
        // cnc-ddraw may wake its renderer here: never hold our lock while
        // entering its surface/global critical sections.
        const HRESULT hr = original(dest,dr,source,sr,flags,fx);
        if (FAILED(hr) && db && saved) { Guard guard; db->plane = *saved; }
        return hr;
    }
    using BltFast = HRESULT (WINAPI*)(void*,DWORD,DWORD,void*,RECT*,DWORD);
    HRESULT WINAPI HookBltFast(void* dest,DWORD x,DWORD y,void* source,RECT* sr,DWORD flags) {
        DDSURFACEDESC2 dd{}, sd{};
        std::shared_ptr<Buffer> db;
        std::unique_ptr<PixelPlane> saved;
        if (Describe(dest,dd) && Describe(source,sd)) {
            PixelRect s = Rect(sr,sd), d{(int)x,(int)y,(int)x+s.right-s.left,(int)y+s.bottom-s.top};
            if (InBounds(s,sd) && InBounds(d,dd)) {
                Guard guard;
                db = Remember(dest,dd); auto sb = Remember(source,sd);
                db->plane.ValidateNative(db->base,db->pitch);
                sb->plane.ValidateNative(sb->base,sb->pitch);
                saved = std::make_unique<PixelPlane>(db->plane);
                auto sk = sd.ddckCKSrcBlt, dk = dd.ddckCKDestBlt;
                db->plane.Copy(sb==db ? saved.get() : &sb->plane,s,d,sb->base,sb->pitch,db->base,db->pitch,
                    (flags & DDBLTFAST_SRCCOLORKEY) != 0,
                    (unsigned short)sk.dwColorSpaceLowValue,(unsigned short)sk.dwColorSpaceHighValue,
                    (flags & DDBLTFAST_DESTCOLORKEY) != 0,
                    (unsigned short)dk.dwColorSpaceLowValue,(unsigned short)dk.dwColorSpaceHighValue);
            }
        }
        auto original = (BltFast)Original(dest,7);
        HRESULT hr = original(dest,x,y,source,sr,flags);
        if (FAILED(hr) && db && saved) { Guard guard; db->plane = *saved; }
        return hr;
    }
    using Lock = HRESULT (WINAPI*)(void*,RECT*,DDSURFACEDESC2*,DWORD,HANDLE);
    HRESULT WINAPI HookLock(void* object,RECT* rect,DDSURFACEDESC2* output,DWORD flags,HANDLE event) {
        HRESULT hr = ((Lock)Original(object,25))(object,rect,output,flags,event);
        DDSURFACEDESC2 d{};
        if (SUCCEEDED(hr) && Describe(object,d)) {
            Guard guard;
            auto b = Remember(object,d);
            if (!gameCopyDepth) {
                b->plane.ValidateNative(b->base,b->pitch);
                if (!(flags & DDLOCK_READONLY)) b->plane.BeginWrite();
            }
        }
        return hr;
    }
    using Unlock = HRESULT (WINAPI*)(void*,void*);
    HRESULT WINAPI HookUnlock(void* object,void* rectOrAddress) {
        {
            Guard guard;
            auto found = state->surfaces.find(object);
            if (!gameCopyDepth && found != state->surfaces.end() && found->second.buffer) {
                auto& s = found->second;
                s.buffer->plane.ValidateNative(s.buffer->base,s.buffer->pitch);
            }
        }
        return ((Unlock)Original(object,32))(object,rectOrAddress);
    }
    using ReleaseDC = HRESULT (WINAPI*)(void*,HDC);
    HRESULT WINAPI HookReleaseDC(void* object,HDC dc) {
        DDSURFACEDESC2 d{};
        if (Describe(object,d)) { Guard guard; Remember(object,d)->plane.Clear(Rect(nullptr,d)); }
        return ((ReleaseDC)Original(object,26))(object,dc);
    }
    using Attached = HRESULT (WINAPI*)(void*,void*,void**);
    HRESULT WINAPI HookAttached(void* object,void* caps,void** output) {
        HRESULT hr = ((Attached)Original(object,12))(object,caps,output);
        if (SUCCEEDED(hr) && output) TrackSurface(*output);
        return hr;
    }
    using Query = HRESULT (WINAPI*)(void*,REFIID,void**);
    HRESULT WINAPI HookSurfaceQuery(void* object,REFIID iid,void** output) {
        HRESULT hr = ((Query)Original(object,0))(object,iid,output);
        // cnc-ddraw exposes surface 1..7 with the same first 36 methods.
        if (SUCCEEDED(hr) && output && *output) TrackSurface(*output);
        return hr;
    }
    using Release = ULONG (WINAPI*)(void*);
    Release realSurfaceRelease = nullptr;
    ULONG WINAPI HookSurfaceRelease(void* object) {
        // Hook the implementation as well as virtual callers: releasing a
        // primary recursively releases attached buffers inside cnc-ddraw.
        ULONG refs = realSurfaceRelease(object);
        if (!refs) {
            Guard guard;
            state->surfaces.erase(object);
            if (state->primary == object) state->primary = nullptr;
            // Flip swaps storage addresses. Reclaim only buffers with no
            // remaining owners; don't confuse a COM pointer with its pixels.
            for (auto it = state->buffers.begin(); it != state->buffers.end();) {
                if (it->second.use_count() == 1) it = state->buffers.erase(it); else ++it;
            }
            state->stats.surfaces = (LONG)state->surfaces.size();
        }
        return refs;
    }
    void TrackSurface(void* object) {
        if (!object) return;
        if (!realSurfaceRelease) {
            void* target=(*(void***)object)[2];
            if (MH_CreateHook(target,(void*)HookSurfaceRelease,(void**)&realSurfaceRelease)!=MH_OK ||
                MH_EnableHook(target)!=MH_OK) {
                Disable("surface lifetime detour failed");
                return;
            }
        }
        Patch(object,0,(void*)HookSurfaceQuery);
        Patch(object,5,(void*)HookBlt);
        Patch(object,7,(void*)HookBltFast);
        Patch(object,12,(void*)HookAttached);
        Patch(object,25,(void*)HookLock);
        Patch(object,26,(void*)HookReleaseDC);
        Patch(object,32,(void*)HookUnlock);
        DDSURFACEDESC2 d{};
        if (Describe(object,d)) { Guard guard; Remember(object,d); }
    }
    using CreateSurface = HRESULT (WINAPI*)(void*,void*,void**,IUnknown*);
    HRESULT WINAPI HookCreateSurface(void* object,void* desc,void** output,IUnknown* outer) {
        HRESULT hr = ((CreateSurface)Original(object,6))(object,desc,output,outer);
        if (SUCCEEDED(hr) && output) TrackSurface(*output);
        return hr;
    }
    HRESULT WINAPI HookDDQuery(void* object,REFIID iid,void** output) {
        HRESULT hr = ((Query)Original(object,0))(object,iid,output);
        if (SUCCEEDED(hr) && output && *output) {
            Patch(*output,0,(void*)HookDDQuery);
            Patch(*output,6,(void*)HookCreateSurface);
        }
        return hr;
    }
    std::shared_ptr<Buffer> Primary() {
        void* object;
        { Guard guard; object = state->primary; }
        DDSURFACEDESC2 d{};
        // Rendering is already inside cnc-ddraw's own global lock. Refresh
        // the public descriptor, including the address swapped by Flip.
        if (!Describe(object,d)) return nullptr;
        Guard guard;
        return Remember(object,d);
    }
    void RecordFrame(const uint32_t* pixels,int width,int height,int pitch) {
        ++state->stats.frames;
        if(state->profile && state->stats.frames%300==0)
            Log::Note("Present32 perf: frames=%ld glyph=%.3fms/frame copy=%.3fms/frame upload=%.3fms/upload overlay=%.3fms/draw",
                state->stats.frames,state->stats.glyphMs/state->stats.frames,state->stats.copyMs/state->stats.frames,
                state->stats.uploadMs/std::max(1L,state->stats.uploads),state->stats.overlayMs/std::max(1L,state->stats.overlays));
#ifdef VT_PRESENT_TEST
        state->lastWidth = width; state->lastHeight = height;
        state->last.resize((size_t)width*height);
        for (int y=0;y<height;++y)
            memcpy(state->last.data()+(size_t)y*width,pixels+(size_t)y*pitch,(size_t)width*4);
#else
        (void)pixels; (void)width; (void)height; (void)pitch;
#endif
    }
    // GDI keeps its destination rectangle and source orientation, but consumes
    // a 32-bit DIB. These detours are strictly scoped to cnc-ddraw callers.
    using Stretch = int (WINAPI*)(HDC,int,int,int,int,int,int,int,int,const void*,const BITMAPINFO*,UINT,DWORD);
    using SetDIB = int (WINAPI*)(HDC,int,int,DWORD,DWORD,int,int,UINT,UINT,const void*,const BITMAPINFO*,UINT);
    Stretch realStretch = nullptr; SetDIB realSetDIB = nullptr;
    bool Dib(const void* pixels,const BITMAPINFO* info,std::vector<uint32_t>& out,BITMAPINFO& converted) {
        if (!info || info->bmiHeader.biBitCount != 16 || info->bmiHeader.biWidth <= 0) return false;
        Guard guard;
        const auto found = state->buffers.find((void*)pixels);
        if (found == state->buffers.end()) return false;
        auto b = found->second;
        b->plane.ValidateNative(b->base,b->pitch);
        if (info->bmiHeader.biWidth != b->width || abs(info->bmiHeader.biHeight) != b->height) return false;
        std::vector<uint32_t> top((size_t)b->width*b->height);
        b->plane.CompositeRect(b->base,b->pitch,top.data(),b->width,{0,0,b->width,b->height});
        RecordFrame(top.data(),b->width,b->height,b->width);
        out.resize(top.size());
        for (int y=0;y<b->height;++y) memcpy(out.data()+(size_t)y*b->width,
            top.data()+(size_t)(info->bmiHeader.biHeight > 0 ? b->height-1-y : y)*b->width,(size_t)b->width*4);
        converted = {}; converted.bmiHeader = info->bmiHeader;
        converted.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        converted.bmiHeader.biBitCount = 32; converted.bmiHeader.biCompression = BI_RGB;
        converted.bmiHeader.biClrUsed = 0; converted.bmiHeader.biClrImportant = 0;
        converted.bmiHeader.biSizeImage = (DWORD)out.size()*4;
        state->stats.backend = 1;
        return true;
    }
    int WINAPI HookStretch(HDC dc,int x,int y,int w,int h,int sx,int sy,int sw,int sh,
                           const void* bits,const BITMAPINFO* bmi,UINT usage,DWORD rop) {
        std::vector<uint32_t> out; BITMAPINFO info{};
        if (FromCnc(_ReturnAddress()) && usage == DIB_RGB_COLORS && Dib(bits,bmi,out,info))
            return realStretch(dc,x,y,w,h,sx,sy,sw,sh,out.data(),&info,usage,rop);
        return realStretch(dc,x,y,w,h,sx,sy,sw,sh,bits,bmi,usage,rop);
    }
    int WINAPI HookSetDIB(HDC dc,int x,int y,DWORD w,DWORD h,int sx,int sy,UINT start,UINT lines,
                         const void* bits,const BITMAPINFO* bmi,UINT usage) {
        std::vector<uint32_t> out; BITMAPINFO info{};
        if (FromCnc(_ReturnAddress()) && usage == DIB_RGB_COLORS && Dib(bits,bmi,out,info))
            return realSetDIB(dc,x,y,w,h,sx,sy,start,lines,out.data(),&info,usage);
        return realSetDIB(dc,x,y,w,h,sx,sy,start,lines,bits,bmi,usage);
    }
    // D3D9: replace only cnc-ddraw's RGB565 upload textures. cnc-ddraw still
    // writes to a 16-bit staging rectangle; Unlock converts it and blends text
    // straight into the actual A8R8G8B8 texture (never back into RGB565).
    using Create9 = IDirect3D9* (WINAPI*)(UINT);
    Create9 realCreate9 = nullptr;
    using Device = HRESULT (WINAPI*)(void*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,void**);
    using CreateTexture = HRESULT (WINAPI*)(void*,UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,void**,HANDLE*);
    using TextureLock = HRESULT (WINAPI*)(void*,UINT,D3DLOCKED_RECT*,const RECT*,DWORD);
    using TextureUnlock = HRESULT (WINAPI*)(void*,UINT);
    using Primitive = HRESULT (WINAPI*)(void*,D3DPRIMITIVETYPE,UINT,UINT);
    using Reset = HRESULT (WINAPI*)(void*,D3DPRESENT_PARAMETERS*);
    bool ResizeAtlas(Texture& t,size_t count)
    {
        if(t.overlay && count<=(size_t)t.atlasColumns*(t.atlasHeight/32)) return true;
        D3DCAPS9 caps{};
        if(!t.device || FAILED(t.device->GetDeviceCaps(&caps))) return false;
        int columns=16, height=32;
        auto needed=[&]() { return (int)((std::max<size_t>(1,count)+columns-1)/columns)*32; };
        while(needed()>(int)caps.MaxTextureHeight && columns*128<=(int)caps.MaxTextureWidth) columns*=2;
        while(height<needed()) height*=2;
        const int width=columns*64;
        if(width>(int)caps.MaxTextureWidth || height>(int)caps.MaxTextureHeight) return false;
        if(t.overlay && columns==t.atlasColumns && height<=t.atlasHeight) return true;
        IDirect3DTexture9* atlas=nullptr;
        auto create=(CreateTexture)Original(t.device,23);
        if(FAILED(create(t.device,width,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,(void**)&atlas,nullptr))) return false;
        if(t.overlay) t.overlay->Release();
        t.overlay=atlas;t.atlasWidth=width;t.atlasHeight=height;t.atlasColumns=columns;
        Log::Note("Present32: sparse text atlas %dx%d (%.2f MiB)",width,height,width*height/262144.0);
        return true;
    }

    bool UploadOverlay(Texture& t,const PixelPlane& plane)
    {
        auto tiles=plane.TextTiles();
        if(tiles.empty()) { t.overlayTiles.clear();return true; }
        if(!ResizeAtlas(t,tiles.size())) return false;
        D3DLOCKED_RECT lock{};
        if(FAILED(t.overlay->LockRect(0,&lock,nullptr,D3DLOCK_NO_DIRTY_UPDATE))) return false;
        // Repack live tiles into a compact atlas. The draw list references only
        // this upload's cells, so abandoned cells never leave visible trails.
        for(size_t i=0;i<tiles.size();++i) {
            const int x=(int)(i%t.atlasColumns)*64,y=(int)(i/t.atlasColumns)*32;
            auto* pixels=(uint32_t*)((char*)lock.pBits+y*lock.Pitch)+x;
            for(int row=0;row<32;++row) memset(pixels+(size_t)row*(lock.Pitch/4),0,64*4);
            plane.Overlay2Rect(pixels,lock.Pitch/4,tiles[i]);
        }
        if(FAILED(t.overlay->UnlockRect(0))) return false;
        RECT dirty{0,0,t.atlasWidth,(LONG)((tiles.size()+t.atlasColumns-1)/t.atlasColumns)*32};
        if(FAILED(t.overlay->AddDirtyRect(&dirty))) return false;
        t.overlayTiles=std::move(tiles);
        return true;
    }

    void AtlasVertices(Texture& t,const ScreenVertex (&quad)[4],float sx,float sy)
    {
        t.overlayVertices.clear();t.overlayVertices.reserve(t.overlayTiles.size()*6);
        const float sl=quad[1].u*t.width,st=quad[1].v*t.height;
        const float sr=quad[3].u*t.width,sb=quad[0].v*t.height;
        for(size_t i=0;i<t.overlayTiles.size();++i) {
            const auto r=t.overlayTiles[i];
            const float l=std::max((float)r.left,sl),top=std::max((float)r.top,st);
            const float right=std::min((float)r.right,sr),bottom=std::min((float)r.bottom,sb);
            if(l>=right || top>=bottom) continue;
            const int ax=(int)(i%t.atlasColumns)*64,ay=(int)(i/t.atlasColumns)*32;
            const float x0=quad[1].x+(l-sl)*sx,x1=quad[1].x+(right-sl)*sx;
            const float y0=quad[1].y+(top-st)*sy,y1=quad[1].y+(bottom-st)*sy;
            const float u0=(ax+(l-r.left)*2)/t.atlasWidth,u1=(ax+(right-r.left)*2)/t.atlasWidth;
            const float v0=(ay+(top-r.top)*2)/t.atlasHeight,v1=(ay+(bottom-r.top)*2)/t.atlasHeight;
            const ScreenVertex a{x0,y0,0,1,u0,v0},b{x1,y0,0,1,u1,v0},c{x0,y1,0,1,u0,v1},d{x1,y1,0,1,u1,v1};
            t.overlayVertices.insert(t.overlayVertices.end(),{a,b,c,b,d,c});
        }
    }

    bool ScreenQuad(IDirect3DDevice9* device,UINT start,ScreenVertex (&vertices)[4])
    {
        DWORD fvf=0;IDirect3DVertexBuffer9* buffer=nullptr;UINT offset=0,stride=0;
        if(FAILED(device->GetFVF(&fvf)) || fvf!=(D3DFVF_XYZRHW|D3DFVF_TEX1) ||
            FAILED(device->GetStreamSource(0,&buffer,&offset,&stride)) || !buffer) return false;
        void* data=nullptr;
        const bool ok=stride==sizeof(ScreenVertex) &&
            SUCCEEDED(buffer->Lock(offset+start*stride,sizeof(vertices),&data,D3DLOCK_READONLY));
        if(ok) { memcpy(vertices,data,sizeof(vertices));buffer->Unlock(); }
        buffer->Release(); return ok;
    }

    // The game quad reflects cnc-ddraw's effective configuration, including
    // borderless/exclusive fullscreen, integer boxing, and viewport offsets.
    HRESULT WINAPI HookPrimitive(void* object,D3DPRIMITIVETYPE type,UINT start,UINT count)
    {
        const auto original=(Primitive)Original(object,81);
        if(!state->autoTextScale || !FromCnc(_ReturnAddress()) || type!=D3DPT_TRIANGLESTRIP || count!=2)
            return original(object,type,start,count);
        auto* device=(IDirect3DDevice9*)object;
        IDirect3DBaseTexture9* texture=nullptr;
        ScreenVertex vertices[4]{};
        if(FAILED(device->GetTexture(0,&texture)) || !texture) return original(object,type,start,count);
        const bool quad=ScreenQuad(device,start,vertices);
        auto primary=Primary();
        bool overlay=false;IDirect3DStateBlock9* saved=nullptr;
        {
            Guard guard;
            auto it=state->textures.find(texture);
            if(quad && primary && it!=state->textures.end()) {
                auto& t=it->second;
                const float sourceW=(vertices[2].u-vertices[0].u)*t.width;
                const float sourceH=(vertices[0].v-vertices[1].v)*t.height;
                const float sx=sourceW>0 ? (vertices[2].x-vertices[0].x)/sourceW : 0;
                const float sy=sourceH>0 ? (vertices[0].y-vertices[1].y)/sourceH : 0;
                const bool doubled=fabs(sx-2.0f)<0.01f && fabs(sy-2.0f)<0.01f;
                if(doubled && !t.overlayAttempted) {
                    t.overlayAttempted=true;
                    t.device=device;
                    if(ResizeAtlas(t,primary->plane.TileCount())) {
                        t.overlayReady=true;
                        Log::Note("Present32: output text scale=2 viewport=%.0fx%.0f logical=%dx%d",
                            vertices[2].x-vertices[0].x,vertices[0].y-vertices[1].y,primary->width,primary->height);
                    }
                    if(!t.overlay) Log::Note("Present32: output text scale=1 (2x texture unavailable)");
                }
                // Only frames whose world upload excluded its old text get an
                // overlay. First frame and any failure keep the 1x composition.
                const auto overlayStart=Stamp();
                overlay=t.cleanWorld && t.overlay && !t.overlayTiles.empty() && SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL,&saved));
                t.overlayReady=doubled && t.overlay;
                if(overlay) {
                    const HRESULT hr=original(object,type,start,count);
                    device->SetPixelShader(nullptr);device->SetVertexShader(nullptr);
                    device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
                    device->SetTexture(0,t.overlay);device->SetTexture(1,nullptr);
                    device->SetRenderState(D3DRS_ZENABLE,FALSE);device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
                    device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
                    device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE);
                    device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
                    device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
                    device->SetRenderState(D3DRS_SRGBWRITEENABLE,state->options.linear);
                    device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,state->options.linear);
                    device->SetSamplerState(0,D3DSAMP_MAGFILTER,doubled ? D3DTEXF_POINT : D3DTEXF_LINEAR);
                    device->SetSamplerState(0,D3DSAMP_MINFILTER,doubled ? D3DTEXF_POINT : D3DTEXF_LINEAR);
                    device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
                    device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
                    device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
                    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
                    device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
                    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
                    device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
                    device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
                    AtlasVertices(t,vertices,sx,sy);
                    if(!t.overlayVertices.empty())
                        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,(UINT)t.overlayVertices.size()/3,t.overlayVertices.data(),sizeof(ScreenVertex));
                    saved->Apply();saved->Release();
                    state->stats.overlayMs+=Elapsed(overlayStart);++state->stats.overlays;
                    texture->Release();return hr;
                }
            }
        }
        texture->Release();return original(object,type,start,count);
    }

    HRESULT WINAPI HookReset(void* object,D3DPRESENT_PARAMETERS* pp)
    {
        const auto original=(Reset)Original(object,16);
        D3DPRESENT_PARAMETERS converted{};
        if(pp && pp->BackBufferFormat==D3DFMT_R5G6B5 && state->enabled) {
            converted=*pp;converted.BackBufferFormat=D3DFMT_X8R8G8B8;
            const HRESULT hr=original(object,&converted);
            if(SUCCEEDED(hr)) *pp=converted;
            return hr;
        }
        return original(object,pp);
    }
    HRESULT WINAPI HookTextureLock(void* object,UINT level,D3DLOCKED_RECT* out,const RECT* rect,DWORD flags) {
        auto original = (TextureLock)Original(object,19);
        HRESULT hr = original(object,level,out,rect,flags);
        if (FAILED(hr) || level || !out) return hr;
        Guard guard;
        auto it = state->textures.find(object);
        if (it == state->textures.end()) return hr;
        auto& t = it->second;
        t.rect = rect ? *rect : RECT{0,0,t.width,t.height};
        t.native = *out;
        int w = t.rect.right-t.rect.left, h = t.rect.bottom-t.rect.top;
        t.scratch.resize((size_t)w*h);
        t.locked = true;
        out->pBits = t.scratch.data(); out->Pitch = w*2;
        return hr;
    }
    HRESULT WINAPI HookTextureUnlock(void* object,UINT level) {
        auto original = (TextureUnlock)Original(object,20);
        bool tracked;
        {
            Guard guard;
            tracked=!level && state->textures.find(object)!=state->textures.end();
        }
        if(!tracked) return original(object,level);
        auto primary = Primary();
        {
            Guard guard;
            auto it = state->textures.find(object);
            if (!level && it != state->textures.end() && it->second.locked) {
                auto& t = it->second;
                const int w=t.rect.right-t.rect.left,h=t.rect.bottom-t.rect.top;
                auto* output = (uint32_t*)t.native.pBits;
                // Native updates use a full game-sized rectangle beginning at
                // zero; unsupported partial updates still get valid 32-bit RGB.
                if (primary && t.rect.left == 0 && t.rect.top == 0 &&
                    w == primary->width && h == primary->height) {
                    const auto uploadStart=Stamp();
                    primary->plane.ValidateNative(t.scratch.data(),w);
                    t.cleanWorld=t.overlayReady && UploadOverlay(t,primary->plane);
                    if(t.cleanWorld) primary->plane.BackgroundRect(t.scratch.data(),w,output,t.native.Pitch/4,{0,0,w,h});
                    else primary->plane.CompositeRect(t.scratch.data(),w,output,t.native.Pitch/4,{0,0,w,h});
                    state->stats.uploadMs+=Elapsed(uploadStart);++state->stats.uploads;
                    RecordFrame(output,w,h,t.native.Pitch/4);
                } else {
                    t.cleanWorld=false;
                    for(int y=0;y<h;++y) for(int x=0;x<w;++x)
                        output[(size_t)y*(t.native.Pitch/4)+x] = PixelPlane::Expand565(t.scratch[(size_t)y*w+x]);
                }
                t.locked = false;
            }
        }
        return original(object,level);
    }
    ULONG WINAPI HookTextureRelease(void* object) {
        auto original = (Release)Original(object,2);
        ULONG count = original(object);
        if (!count) { Guard guard;
            auto it=state->textures.find(object);
            if(it!=state->textures.end() && it->second.overlay) it->second.overlay->Release();
            state->textures.erase(object);
        }
        return count;
    }
    HRESULT WINAPI HookCreateTexture(void* object,UINT w,UINT h,UINT levels,DWORD usage,
                                    D3DFORMAT format,D3DPOOL pool,void** out,HANDLE* shared) {
        auto original = (CreateTexture)Original(object,23);
        if (state->enabled && FromCnc(_ReturnAddress()) && format == D3DFMT_R5G6B5 && levels == 1 && !shared) {
            HRESULT hr = original(object,w,h,levels,usage,D3DFMT_A8R8G8B8,pool,out,shared);
            if (SUCCEEDED(hr) && out && *out &&
                Patch(*out,19,(void*)HookTextureLock) && Patch(*out,20,(void*)HookTextureUnlock) &&
                Patch(*out,2,(void*)HookTextureRelease)) {
                Guard guard;
                state->textures.emplace(*out,Texture{(int)w,(int)h});
                state->stats.backend = 2;
                Log::Note("Present32: D3D9 upload texture %ux%u promoted to BGRA8",w,h);
                return hr;
            }
            if (SUCCEEDED(hr) && out && *out) { (*(IDirect3DTexture9**)out)->Release(); *out=nullptr; }
            Disable("D3D9 texture promotion failed");
        }
        return original(object,w,h,levels,usage,format,pool,out,shared);
    }
    HRESULT WINAPI HookCreateDevice(void* object,UINT adapter,D3DDEVTYPE type,HWND window,DWORD flags,
                                    D3DPRESENT_PARAMETERS* pp,void** out) {
        auto original=(Device)Original(object,16);
        const bool cnc=FromCnc(_ReturnAddress());
        D3DPRESENT_PARAMETERS parameters{};
        D3DPRESENT_PARAMETERS* actual=pp;
        if(cnc && pp && pp->BackBufferFormat==D3DFMT_R5G6B5) {
            parameters=*pp; parameters.BackBufferFormat=D3DFMT_X8R8G8B8; actual=&parameters;
        }
        // State getters are unavailable on pure devices. Needed only for the
        // opt-in automatic overlay, to preserve cnc-ddraw's quad and all state.
        if(cnc && state->autoTextScale) flags&=~D3DCREATE_PUREDEVICE;
        HRESULT hr = original(object,adapter,type,window,flags,actual,out);
        if(FAILED(hr) && actual!=pp) {
            Disable("D3D9 32-bit backbuffer unavailable");
            actual=pp;
            hr=original(object,adapter,type,window,flags,pp,out);
        }
        if(SUCCEEDED(hr) && actual!=pp) *pp=parameters;
        if (SUCCEEDED(hr) && out && cnc) {
            Patch(*out,23,(void*)HookCreateTexture);
            Patch(*out,16,(void*)HookReset);
            if(state->autoTextScale && state->options.linear) {
                D3DCAPS9 caps{};
                auto* api=(IDirect3D9*)object;
                auto* device=(IDirect3DDevice9*)*out;
                // Linear premultiplied ink needs sRGB conversion after blending.
                // Older adapters retain the already verified 1x compositor.
                HRESULT capResult=device->GetDeviceCaps(&caps);
                HRESULT readResult=api->CheckDeviceFormat(adapter,type,actual->BackBufferFormat,
                    D3DUSAGE_QUERY_SRGBREAD,D3DRTYPE_TEXTURE,D3DFMT_A8R8G8B8);
                HRESULT writeResult=api->CheckDeviceFormat(adapter,type,actual->BackBufferFormat,
                    D3DUSAGE_RENDERTARGET|D3DUSAGE_QUERY_SRGBWRITE,D3DRTYPE_TEXTURE,actual->BackBufferFormat);
                if(FAILED(capResult) || !(caps.PrimitiveMiscCaps&D3DPMISCCAPS_POSTBLENDSRGBCONVERT) ||
                    FAILED(readResult) || FAILED(writeResult)) {
                    state->autoTextScale=false;
                    Log::Note("Present32: output text scale=1 (linear overlay caps=%08lX read=%08lX write=%08lX)",
                        caps.PrimitiveMiscCaps,readResult,writeResult);
                }
            }
            if(state->autoTextScale && !Patch(*out,81,(void*)HookPrimitive)) state->autoTextScale=false;
        }
        return hr;
    }
    IDirect3D9* WINAPI HookCreate9(UINT version) {
        IDirect3D9* object = realCreate9(version);
        if (object) Patch(object,16,(void*)HookCreateDevice);
        return object;
    }
    // OpenGL 1.1 entry points are exported by opengl32; extension loading is
    // irrelevant for the upload itself. Preserve all unpack state.
    using TexImage = void (APIENTRY*)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
    using TexSub = void (APIENTRY*)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*);
    using GetInt = void (APIENTRY*)(GLenum,GLint*);
    using PixelStore = void (APIENTRY*)(GLenum,GLint);
    using GetError = GLenum (APIENTRY*)();
    TexImage realImage = nullptr; TexSub realSub = nullptr; GetInt glGetInt = nullptr; PixelStore glStore = nullptr;
    GetError glError = nullptr;
    void APIENTRY HookImage(GLenum target,GLint level,GLint internal,GLsizei w,GLsizei h,GLint border,
                            GLenum format,GLenum type,const void* pixels) {
        if (state->enabled && glError && FromCnc(_ReturnAddress()) && target == GL_TEXTURE_2D && level == 0 &&
            format == GL_RGB && type == 0x8363 && !pixels) {
            realImage(target,level,GL_RGBA8,w,h,border,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            if(glError()==GL_NO_ERROR) { Guard guard; state->stats.backend = 3; return; }
            Disable("OpenGL BGRA8 texture unavailable");
        }
        realImage(target,level,internal,w,h,border,format,type,pixels);
    }
    void APIENTRY HookSub(GLenum target,GLint level,GLint x,GLint y,GLsizei w,GLsizei h,
                          GLenum format,GLenum type,const void* pixels) {
        std::vector<uint32_t> out;
        if (FromCnc(_ReturnAddress()) && target == GL_TEXTURE_2D && level == 0 && format == GL_RGB &&
            type == 0x8363 && pixels && glGetInt && glStore) {
            GLint row=0,skipRows=0,skipPixels=0,alignment=0,swap=0;
            glGetInt(GL_UNPACK_ROW_LENGTH,&row); glGetInt(GL_UNPACK_SKIP_ROWS,&skipRows);
            glGetInt(GL_UNPACK_SKIP_PIXELS,&skipPixels); glGetInt(GL_UNPACK_ALIGNMENT,&alignment);
            glGetInt(GL_UNPACK_SWAP_BYTES,&swap);
            {
                Guard guard;
                auto it=state->buffers.find((void*)pixels);
                if(it!=state->buffers.end() && w==it->second->width && h==it->second->height &&
                    !skipRows && !skipPixels && !swap &&
                    ((!row && ((w*2+alignment-1)/alignment*alignment)==it->second->pitch*2) ||
                     row==it->second->pitch)) {
                    out.resize((size_t)w*h);
                    it->second->plane.ValidateNative((const unsigned short*)pixels,it->second->pitch);
                    it->second->plane.CompositeRect(it->second->base,it->second->pitch,out.data(),w,{0,0,w,h});
                    RecordFrame(out.data(),w,h,w);
                }
            }
            if (!out.empty()) {
                glStore(GL_UNPACK_ROW_LENGTH,0); glStore(GL_UNPACK_ALIGNMENT,4);
                realSub(target,level,x,y,w,h,0x80E1,GL_UNSIGNED_BYTE,out.data());
                glStore(GL_UNPACK_ROW_LENGTH,row); glStore(GL_UNPACK_ALIGNMENT,alignment);
                return;
            }
        }
        realSub(target,level,x,y,w,h,format,type,pixels);
    }
    FARPROC WINAPI HookProc(HMODULE module,LPCSTR name) {
        FARPROC result = realProc(module,name);
        if (!FromCnc(_ReturnAddress()) || (uintptr_t)name <= 0xFFFF) return result;
        if (!strcmp(name,"Direct3DCreate9")) { realCreate9=(Create9)result; return (FARPROC)HookCreate9; }
        return result;
    }
    bool Detour(HMODULE module,const char* name,void* replacement,void** original) {
        if (!module) return false;
        void* target=(void*)realProc(module,name);
        if (!target || MH_CreateHook(target,replacement,original)!=MH_OK) return false;
        return MH_QueueEnableHook(target)==MH_OK;
    }
    void Start(HMODULE module) {
        if (state) return;
        state = new State;
        if (!Cfg::ConfigBool("Enabled",true) || !Cfg::ConfigBool("Present32",true) ||
            Cfg::Mode()!=Cfg::Mode_Draw) return;
        cncModule=module;
        auto dos=(IMAGE_DOS_HEADER*)module;
        auto nt=(IMAGE_NT_HEADERS*)((char*)module+dos->e_lfanew);
        cncEnd=(uintptr_t)module+nt->OptionalHeader.SizeOfImage;
        state->options.linear=Cfg::LinearBlend(); state->options.antialias=Cfg::AntiAlias();
        state->options.gamma=Cfg::Gamma(); state->options.outline=Cfg::Outline();
        state->options.outlineColor=Cfg::OutlineColor();
        state->autoTextScale=Cfg::HiDPI();
        state->profile=Cfg::ConfigBool("PresentProfile",false);
        state->options.highResolution=state->autoTextScale;
        if (MH_Initialize()!=MH_OK) { Log::Note("Present32: MinHook initialization failed"); return; }
        HMODULE kernel=GetModuleHandleW(L"kernel32.dll"),gdi=GetModuleHandleW(L"gdi32.dll");
        const bool proc=Detour(kernel,"GetProcAddress",(void*)HookProc,(void**)&realProc);
        const bool stretch=Detour(gdi,"StretchDIBits",(void*)HookStretch,(void**)&realStretch);
        const bool set=Detour(gdi,"SetDIBitsToDevice",(void*)HookSetDIB,(void**)&realSetDIB);
        HMODULE executable=GetModuleHandleW(nullptr);
        char filename[MAX_PATH]{}; GetModuleFileNameA(executable,filename,MAX_PATH);
        const char* basename=strrchr(filename,'\\');
        if (!_stricmp(basename ? basename+1 : filename,"gamemd.exe")) {
            const unsigned char prefix[]={0x81,0xEC,0xB4,0,0,0,0x53,0x56,0x8B,0xF1,0x57};
            const unsigned char copyPrefix[]={0x8B,0x44,0x24,0x1C,0x83,0xEC,0x20,0x53,0x56,0x8B,0xF1};
            auto exeDos=(IMAGE_DOS_HEADER*)executable;
            auto exeNt=(IMAGE_NT_HEADERS*)((char*)executable+exeDos->e_lfanew);
            const bool valid=(uintptr_t)executable==0x400000 &&
                exeNt->FileHeader.TimeDateStamp==0x3BDF544E &&
                *(uintptr_t*)0x7E85E4==0x4BB620 &&
                *(uintptr_t*)0x7E8658==0x4C1AB0 &&
                !memcmp((void*)0x4BB620,prefix,sizeof(prefix)) &&
                !memcmp((void*)0x437350,copyPrefix,sizeof(copyPrefix));
            if (!valid ||
                MH_CreateHook((void*)0x4BB620,(void*)HookGameFill,(void**)&realGameFill)!=MH_OK ||
                MH_QueueEnableHook((void*)0x4BB620)!=MH_OK ||
                MH_CreateHook((void*)0x437350,(void*)HookGameCopy,(void**)&realGameCopy)!=MH_OK ||
                MH_QueueEnableHook((void*)0x437350)!=MH_OK) {
                MH_Uninitialize();
                Log::Note("Present32: native fill/copy entry differs; retaining RGB565");
                return;
            }
            // Loading constructs a BSurface (vtable 7E2070) at 552D94..552DCD,
            // stores it at loading-object+60 and deletes it through vtable[0]
            // at 5543EC..5543F7. Scope this optional hook to that verified type.
            const unsigned char deletePrefix[]={0x56,0x8B,0xF1,0x8D,0x4E,0x14};
            if(*(uintptr_t*)0x7E2070==0x411650 &&
                !memcmp((void*)0x411650,deletePrefix,sizeof(deletePrefix)) &&
                MH_CreateHook((void*)0x411650,(void*)HookCpuDelete,(void**)&realCpuDelete)==MH_OK &&
                MH_QueueEnableHook((void*)0x411650)==MH_OK) state->cpuTextReady=true;
            else Log::Note("Present32: BSurface lifetime hook unavailable; CPU text retains RGB565");
        }
        HMODULE ogl=LoadLibraryW(L"opengl32.dll");
        if (ogl) {
            glGetInt=(GetInt)realProc(ogl,"glGetIntegerv"); glStore=(PixelStore)realProc(ogl,"glPixelStorei");
            glError=(GetError)realProc(ogl,"glGetError");
            const bool image=Detour(ogl,"glTexImage2D",(void*)HookImage,(void**)&realImage);
            const bool sub=Detour(ogl,"glTexSubImage2D",(void*)HookSub,(void**)&realSub);
            // Both are needed: promoting allocation without converting later
            // uploads would produce a texture missing the retained text.
            if(!image || !sub) {
                MH_Uninitialize(); Log::Note("Present32: OpenGL upload detours unavailable; retaining RGB565"); return;
            }
        }
        if (!proc || !stretch || !set || MH_ApplyQueued()!=MH_OK) {
            MH_Uninitialize(); Log::Note("Present32: detour setup failed; retaining RGB565"); return;
        }
        state->enabled=true;
        SetPresentationWriter(Draw);
        Log::Note("Present32: cnc-ddraw detected; waiting for BGRA8 presenter");
    }
    HRESULT WINAPI HookCreateDD(GUID* guid,void** out,IUnknown* outer) {
        HMODULE module=GetModuleHandleW(L"ddraw.dll");
        // Two distinctive exports, not merely a filename: native DirectDraw
        // and other wrappers must retain the existing rendering path.
        if (module && importedProc(module,"pvBmpBits") && importedProc(module,"GameHandlesClose")) Start(module);
        HRESULT hr=realCreate(guid,out,outer);
        if (SUCCEEDED(hr) && out && state && state->enabled) {
            Patch(*out,0,(void*)HookDDQuery); Patch(*out,6,(void*)HookCreateSurface);
        }
        return hr;
    }
    FARPROC WINAPI EarlyProc(HMODULE module,LPCSTR name) {
        FARPROC result=importedProc(module,name);
        if ((uintptr_t)name>0xFFFF && !strcmp(name,"DirectDrawCreate") &&
            importedProc(module,"pvBmpBits") && importedProc(module,"GameHandlesClose")) {
            realCreate=(CreateDD)result; return (FARPROC)HookCreateDD;
        }
        return result;
    }
    void Import(void** slot,void* replacement) {
        DWORD protection;
        if (VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection)) {
            InterlockedExchangePointer(slot,replacement);
            DWORD ignored; VirtualProtect(slot,sizeof(void*),protection,&ignored);
        }
    }
} // anonymous
    void PrepareEarly() {
        // Syringe temporarily loads this DLL in its own process to read the
        // handshake, then unloads it. Never leave imports pointing into it.
#ifndef VT_PRESENT_TEST
        char executable[MAX_PATH]{};
        GetModuleFileNameA(nullptr,executable,MAX_PATH);
        const char* filename=strrchr(executable,'\\');
        if (_stricmp(filename ? filename+1 : executable,"gamemd.exe")) return;
#endif
        auto base=(unsigned char*)GetModuleHandleW(nullptr);
        auto dos=(IMAGE_DOS_HEADER*)base;
        auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
        DWORD rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!rva) return;
        auto descriptor=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);
        for (;descriptor->Name;++descriptor) {
            if (!descriptor->OriginalFirstThunk) continue;
            auto names=(IMAGE_THUNK_DATA*)(base+descriptor->OriginalFirstThunk);
            auto addresses=(IMAGE_THUNK_DATA*)(base+descriptor->FirstThunk);
            for (;names->u1.AddressOfData;++names,++addresses) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const char* name=(const char*)((IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData))->Name;
                void** slot=(void**)&addresses->u1.Function;
                if (!strcmp(name,"DirectDrawCreate")) {
                    realCreate=(CreateDD)*slot; Import(slot,(void*)HookCreateDD);
                } else if (!strcmp(name,"GetProcAddress")) {
                    importedProc=(Proc)*slot; Import(slot,(void*)EarlyProc);
                }
            }
        }
    }
    void TrackTextSurface(void* surface) {
        if(!state || !state->enabled || !state->cpuTextReady || !surface) return;
        // Only the verified native BSurface class owns this memory layout.
        // All DSurface and extension surface paths keep their existing hooks.
        if(*(uintptr_t*)surface!=0x7E2070) return;
        TrackCpuText(surface);
    }
    Statistics Stats() {
        if (!state) return {};
        Guard guard; return state->stats;
    }
#ifdef VT_PRESENT_TEST
    void TestCpuTextStart() {
        state=new State();state->enabled=true;state->cpuTextReady=true;
        state->options.highResolution=true;
        // Deliberately leave backend=0: loading can precede texture creation.
        SetPresentationWriter(Draw);
    }
    void TestCpuTextTrack(void* surface) { TrackCpuText(surface); }
    void TestCpuTextRelease(void* surface) { ForgetCpuText(surface); }
    bool TestCpuTextOverlay(void* surface,unsigned int* output,int pitch) {
        DDSURFACEDESC2 d{};void* native=nullptr;
        if(!GameDescription(surface,d,native)) return false;
        Guard guard;
        const auto found=state->buffers.find(d.lpSurface);
        if(found==state->buffers.end()) return false;
        auto& b=*found->second;
        b.plane.ValidateNative(b.base,b.pitch);
        b.plane.Overlay2Rect(output,pitch,{0,0,b.width,b.height});
        return true;
    }
    unsigned int TestPixel(int x,int y) {
        if (!state) return 0;
        Guard guard;
        return x>=0 && y>=0 && x<state->lastWidth && y<state->lastHeight ?
            state->last[(size_t)y*state->lastWidth+x] : 0;
    }
    void TestGameFill(void* gameSurface,const int* clip,const int* rect,DWORD color,TestFill original) {
        realGameFill=original;
        HookGameFill(gameSurface,nullptr,clip,rect,color);
    }
    bool TestGameCopy(void* dest,const int* dr,void* source,const int* sr,void* copier,TestCopy original) {
        realGameCopy=original;
        return HookGameCopy(dest,dr,source,sr,copier,0,3,1000,0);
    }
#endif
} }
