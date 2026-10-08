// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#ifdef VT_PRESENT_TEST
#include <vector>
#include "PixelPlane.h"
#endif

namespace vt { namespace Presentation32 {
    // Patch only the executable's imports under the loader lock. Actual
    // configuration, allocation and renderer detours wait for DirectDrawCreate.
    void PrepareEarly();
    // BitText's surface argument can be the loading screen's CPU BSurface.
    // Register its text sidecar before BitFont::Lock; DDS targets stay unchanged.
    void TrackTextSurface(void* surface);
    // Explicit unloading is unsupported while renderer threads are running.
    // Process termination needs no restoration of process-local hooks.
    struct Statistics {
        LONG glyphs, frames, surfaces, backend;
        LONG copies, uploads, overlays;
        double glyphMs, copyMs, uploadMs, overlayMs;
    };
    Statistics Stats();
#ifdef VT_PRESENT_TEST
    unsigned int TestPixel(int x, int y);
    using TestFill = bool (__thiscall*)(void*,const int*,const int*,DWORD);
    void TestGameFill(void* gameSurface,const int* clip,const int* rect,DWORD color,TestFill original);
    using TestCopy = bool (__fastcall*)(void*,const int*,void*,const int*,void*,int,int,int,int);
    bool TestGameCopy(void* dest,const int* dr,void* source,const int* sr,void* copier,TestCopy original);
    void TestCpuTextStart();
    void TestCpuTextTrack(void* surface);
    void TestCpuTextRelease(void* surface);
    bool TestCpuTextOverlay(void* surface,unsigned int* output,int pitch);
    bool TestFrameUploadBegin(void* surface,void* texture,void** pixels,int* pitch);
    bool TestFrameUploadEnd(void* texture);
    bool TestSurfaceUnlock(void* surface);
    struct TestOverlay {
        int width=0,height=0;
        bool point=false;
        std::vector<unsigned int> pixels;
        std::vector<float> vertices; // x,y,u,v, six vertices per cell
    };
    bool TestBuildOverlay(const PixelPlane& plane,float sx,float sy,float offsetX,float offsetY,
                          TestOverlay& output,const PixelRect* source=nullptr);
    float TestObservedScale(bool vertical = false);
    int TestObservedRaster();
#endif
} }
