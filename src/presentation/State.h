// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Private presentation state. Native surfaces stay RGB565; each backend consumes
// the same sparse text sidecar. Only Presentation32.h is a public interface.
#include "Presentation32.h"
#include "PixelPlane.h"
#include "PixelWriter.h"
#include "Logger.h"
#include <MinHook.h>
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

namespace vt::Presentation32::detail
{
struct Slot
{
    void **table;
    int index;
    void *original;
};
struct Buffer
{
    unsigned short *base;
    int width, height, pitch;
    PixelPlane plane;
    bool cpuText = false;
    Buffer(const DDSURFACEDESC2 &d, PlaneOptions options)
        : base((unsigned short *)d.lpSurface), width((int)d.dwWidth), height((int)d.dwHeight), pitch(d.lPitch / 2),
          plane(width, height, options)
    {
    }
};
struct Surface
{
    DDSURFACEDESC2 desc{};
    std::shared_ptr<Buffer> buffer;
};
struct ScreenVertex
{
    float x, y, z, rhw, u, v;
};
// Two high-resolution samples per logical axis, with one logical-pixel
// gutter copied from neighbors. Linear filtering must not read another
// atlas cell, nor clamp at an internal text-tile boundary.
constexpr int AtlasCellW = 68, AtlasCellH = 36, AtlasPad = 2;
struct Texture
{
    int width, height;
    D3DLOCKED_RECT native{};
    RECT rect{};
    std::vector<unsigned short> scratch;
    bool locked = false;
    bool cleanWorld = false;
    IDirect3DTexture9 *overlay = nullptr;
    IDirect3DDevice9 *device = nullptr;
    int atlasWidth = 0, atlasHeight = 0, atlasColumns = 16;
    std::vector<ScreenVertex> overlayVertices;
    std::vector<PixelRect> overlayTiles;
    bool overlayReady = false;
    bool overlayAttempted = false;
    float reportedScale = 0;
};
struct State
{
    CRITICAL_SECTION cs;
    std::vector<Slot> slots;
    std::map<void *, Surface> surfaces;
    std::map<void *, std::shared_ptr<Buffer>> buffers;
    std::map<void *, std::shared_ptr<Buffer>> cpuTextSurfaces;
    std::map<void *, Texture> textures;
    PlaneOptions options;
    void *primary = nullptr;
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
    State()
    {
        InitializeCriticalSection(&cs);
        QueryPerformanceFrequency(&frequency);
    }
};

using Proc = FARPROC(WINAPI *)(HMODULE, LPCSTR);
using CreateDD = HRESULT(WINAPI *)(GUID *, void **, IUnknown *);
using Release = ULONG(WINAPI *)(void *);
using CpuDelete = void *(__thiscall *)(void *, unsigned int);
using GameCopy = bool(__fastcall *)(void *, const int *, void *, const int *, void *, int, int, int, int);
using GameFill = bool(__thiscall *)(void *, const int *, const int *, DWORD);
extern State *state;
extern HMODULE cncModule;
extern uintptr_t cncEnd;
extern Proc realProc, importedProc;
extern CreateDD realCreate;
extern CpuDelete realCpuDelete;
extern GameCopy realGameCopy;
extern GameFill realGameFill;
LONGLONG Stamp();
double Elapsed(LONGLONG start);
bool FromCnc(void *caller);
void *Original(void *object, int slot);
bool Patch(void *object, int index, void *replacement);
bool Describe(void *object, DDSURFACEDESC2 &d);
std::shared_ptr<Buffer> Remember(void *object, const DDSURFACEDESC2 &d);
std::shared_ptr<Buffer> Primary();
void RecordFrame(const uint32_t *pixels, int width, int height, int pitch);
int Draw(const Target &t, const GlyphCell &cell, int x, int y, int rows, unsigned short color, bool aa);
void Disable(const char *reason);
bool GameDescription(void *object, DDSURFACEDESC2 &d, void *&native);
void ForgetCpuText(void *object);
void TrackCpuText(void *object);
void *__fastcall HookCpuDelete(void *object, void *, unsigned int flags);
bool __fastcall HookGameCopy(void *dest, const int *dr, void *source, const int *sr, void *copier, int remap, int mode,
                             int amount, int extra);
bool __fastcall HookGameFill(void *object, void *, const int *clip, const int *rect, DWORD color);
HRESULT WINAPI HookCreateSurface(void *object, void *desc, void **output, IUnknown *outer);
HRESULT WINAPI HookDDQuery(void *object, REFIID iid, void **output);
bool Detour(HMODULE module, const char *name, void *replacement, void **original);
bool InstallGdi();
bool InstallOpenGL();
FARPROC ResolveD3D9Create(FARPROC original);
struct Guard
{
    Guard() { EnterCriticalSection(&state->cs); }
    ~Guard() { LeaveCriticalSection(&state->cs); }
};
} // namespace vt::Presentation32::detail
