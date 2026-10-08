// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Presentation32.h"
#include "PixelWriter.h"
#include "Logger.h"
#include "PixelPlane.h"
#include <ddraw.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdlib>

static HWND window;
static void Pump(DWORD duration) {
    DWORD start=GetTickCount();
    while(GetTickCount()-start<duration) {
        MSG msg; while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        Sleep(10);
    }
}
static int errors=0;
static bool __fastcall CpuCopy(void* object,const int* dr,void* source,const int* sr,
                               void* copier,int,int,int,int) {
    auto dest=*(LPDIRECTDRAWSURFACE*)((char*)object+0x1C);
    const bool direct=(uintptr_t)(*(void***)source)[33]==0x4C1AB0;
    auto src=direct ? *(LPDIRECTDRAWSURFACE*)((char*)source+0x1C) : nullptr;
    DDSURFACEDESC d{},s{}; d.dwSize=sizeof(d); s.dwSize=sizeof(s);
    if(src) {
        if(FAILED(src->Lock(nullptr,&s,DDLOCK_WAIT|DDLOCK_READONLY,nullptr))) return false;
    } else {
        s.dwWidth=((int*)source)[1]; s.dwHeight=((int*)source)[2];
        s.lPitch=s.dwWidth*2; s.lpSurface=*(void**)((char*)source+0x14);
    }
    if(FAILED(dest->Lock(nullptr,&d,DDLOCK_WAIT,nullptr))) { if(src) src->Unlock(nullptr); return false; }
    for(int y=0;y<dr[3];++y) for(int x=0;x<dr[2];++x) {
        const int dx=dr[0]+x,dy=dr[1]+y,sx=sr[0]+x,sy=sr[1]+y;
        if(dx<0 || dy<0 || dx>=(int)d.dwWidth || dy>=(int)d.dwHeight ||
           sx<0 || sy<0 || sx>=(int)s.dwWidth || sy>=(int)s.dwHeight) continue;
        unsigned short pixel=((unsigned short*)s.lpSurface)[sy*(s.lPitch/2)+sx];
        if(*(uintptr_t*)copier==0x7F7BC4 || pixel)
            ((unsigned short*)d.lpSurface)[dy*(d.lPitch/2)+dx]=pixel;
    }
    dest->Unlock(nullptr); if(src) src->Unlock(nullptr); return true;
}
static int __fastcall CpuPitch(void* source,void*) { return ((int*)source)[1]*2; }
struct OfflineDDS { void** table; unsigned short* pixels; int width,height; };
static HRESULT WINAPI OfflineDesc(void* object,DDSURFACEDESC2* d) {
    const auto& s=*(OfflineDDS*)object;
    *d={};d->dwSize=sizeof(*d);d->dwWidth=s.width;d->dwHeight=s.height;
    d->lPitch=s.width*2;d->lpSurface=s.pixels;
    d->ddpfPixelFormat.dwRGBBitCount=16;d->ddpfPixelFormat.dwRBitMask=0xF800;
    d->ddpfPixelFormat.dwGBitMask=0x7E0;d->ddpfPixelFormat.dwBBitMask=0x1F;
    return S_OK;
}
static bool __fastcall OfflineCopy(void* dest,const int* dr,void* source,const int* sr,
                                  void* copier,int,int,int,int) {
    auto* d=*(OfflineDDS**)((char*)dest+0x1C);
    const auto* pixels=*(unsigned short**)((char*)source+0x14);
    const int width=((int*)source)[1];
    for(int y=0;y<dr[3];++y) for(int x=0;x<dr[2];++x) {
        const auto pixel=pixels[(sr[1]+y)*width+sr[0]+x];
        if(*(uintptr_t*)copier==0x7F7BC4 || pixel)
            d->pixels[(dr[1]+y)*d->width+dr[0]+x]=pixel;
    }
    return true;
}
static bool __fastcall FailedCopy(void*,const int*,void*,const int*,void*,int,int,int,int) { return false; }
struct OfflineTexture { void** table; unsigned int pixels[32*24]{}; };
static HRESULT WINAPI OfflineTextureLock(void* object,UINT,D3DLOCKED_RECT* out,const RECT*,DWORD) {
    out->pBits=((OfflineTexture*)object)->pixels;out->Pitch=32*4;return S_OK;
}
static HRESULT WINAPI OfflineTextureUnlock(void*,UINT) { return S_OK; }
static unsigned short unlockExpected=0;
static bool unlockObserved=false;
static HRESULT WINAPI OfflineSurfaceUnlock(void* object,void*) {
    const auto& d=*(OfflineDDS*)object;
    unlockObserved=d.pixels[4*d.width+4]==unlockExpected;
    return S_OK;
}
static bool __fastcall OfflineCopyAndUnlock(void* dest,const int* dr,void* source,const int* sr,
                                          void* copier,int a,int b,int c,int e) {
    return OfflineCopy(dest,dr,source,sr,copier,a,b,c,e) && vt::Presentation32::TestSurfaceUnlock(dest);
}
static void GameSurface(unsigned char* memory,void** table,LPDIRECTDRAWSURFACE surface) {
    table[33]=(void*)0x4C1AB0;
    *(void***)memory=table;
    *(LPDIRECTDRAWSURFACE*)(memory+0x1C)=surface;
}
static void Counter(LPDIRECTDRAWSURFACE surface,vt::GlyphSource& font,const char* text,vt::PixelPlane& expected) {
    int width=0;
    for(const char* ch=text;*ch;++ch) width+=font.Get((unsigned char)*ch,-1)->advanceQ;
    int pen=160-width/2;
    DDSURFACEDESC d{}; d.dwSize=sizeof(d); surface->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
    vt::Target target{(unsigned short*)d.lpSurface,d.lPitch/2,0,0,639,479};
    for(const char* ch=text;*ch;++ch) {
        const auto glyph=font.Get((unsigned char)*ch,-1,pen&3);
        vt::DrawCellAA(target,*glyph,80+pen/4,100,16,0xFFFF,vt::RGB565);
        expected.Paint(*glyph,80+pen/4,100,16,0xFFFF,{0,0,640,480});
        pen+=glyph->advanceQ;
    }
    surface->Unlock(nullptr);
}
static bool __fastcall CpuFill(void* object,void*,const int* clip,const int* rect,DWORD color) {
    auto surface=*(LPDIRECTDRAWSURFACE*)((char*)object+0x1C);
    DDSURFACEDESC desc{}; desc.dwSize=sizeof(desc);
    if(FAILED(surface->Lock(nullptr,&desc,DDLOCK_WAIT,nullptr))) return false;
    const int l=std::max(clip[0],rect[0]),t=std::max(clip[1],rect[1]);
    const int r=std::min(clip[0]+clip[2],rect[0]+rect[2]),b=std::min(clip[1]+clip[3],rect[1]+rect[3]);
    for(int y=t;y<b;++y) for(int x=l;x<r;++x)
        ((unsigned short*)desc.lpSurface)[y*(desc.lPitch/2)+x]=(unsigned short)color;
    surface->Unlock(nullptr);
    return true;
}
static void Check(bool ok,const char* message) {
    printf("%s: %s\n",ok?"PASS":"FAIL",message); if(!ok) ++errors;
    // cnc-ddraw can detach the console; retain verification independently.
    FILE* report=fopen("cnc-qa.txt","a");
    if(report) { fprintf(report,"%s: %s\n",ok?"PASS":"FAIL",message);fclose(report); }
}
static double SampleChannel(const std::vector<unsigned int>& pixels,int width,int height,
                            double x,double y,int channel,bool point) {
    auto at=[&](int px,int py) {
        if(px<0 || py<0 || px>=width || py>=height) return 0.0;
        return (double)((pixels[(size_t)py*width+px]>>(channel*8))&255);
    };
    if(point) return at((int)floor(x+0.5),(int)floor(y+0.5));
    const int l=(int)floor(x),top=(int)floor(y);
    const double fx=x-l,fy=y-top;
    return (at(l,top)*(1-fx)+at(l+1,top)*fx)*(1-fy)+
        (at(l,top+1)*(1-fx)+at(l+1,top+1)*fx)*fy;
}
static double AtlasSample(const vt::Presentation32::TestOverlay& frame,int x,int y,int channel) {
    const auto& v=frame.vertices;
    for(size_t i=0;i<v.size();i+=24) {
        // Each six-vertex rectangle has a top-left, top-right and bottom-left.
        if(x<v[i] || x>=v[i+4] || y<v[i+1] || y>=v[i+9]) continue;
        const double u=v[i+2]+(x-v[i])/(v[i+4]-v[i])*(v[i+6]-v[i+2]);
        const double z=v[i+3]+(y-v[i+1])/(v[i+9]-v[i+1])*(v[i+11]-v[i+3]);
        return SampleChannel(frame.pixels,frame.width,frame.height,
            u*frame.width-0.5,z*frame.height-0.5,channel,frame.point);
    }
    return 0;
}
int main(int argc,char** argv) {
    if(argc>1 && !strcmp(argv[1],"--movie-frames")) {
        vt::Presentation32::TestCpuTextStart();
        unsigned char cache[0x24]{},screen[0x24]{};void* cacheTable[34]{},*screenTable[34]{},*ddsTable[34]{},*textureTable[21]{};
        unsigned short cachePixels[32*24]{},screenPixels[32*24]{};
        cacheTable[29]=(void*)CpuPitch;*(void***)cache=cacheTable;
        ((int*)cache)[1]=32;((int*)cache)[2]=24;((int*)cache)[4]=2;*(void**)(cache+0x14)=cachePixels;
        ddsTable[22]=(void*)OfflineDesc;OfflineDDS dds{ddsTable,screenPixels,32,24};
        screenTable[33]=(void*)0x4C1AB0;*(void***)screen=screenTable;*(void**)(screen+0x1C)=&dds;
        textureTable[19]=(void*)OfflineTextureLock;textureTable[20]=(void*)OfflineTextureUnlock;
        OfflineTexture texture{textureTable};
        vt::GlyphRaster2 hi;hi.scale=5;hi.width=hi.rows=5;hi.coverage.assign(25,255);
        vt::GlyphCell glyph{};glyph.inkRows=1;glyph.cov[0]=128;glyph.raster2=&hi;
        vt::Target target{cachePixels,32,0,0,31,23};
        vt::Presentation32::TestCpuTextTrack(cache);vt::BeginTextInkCapture(true);
        vt::DrawCellAA(target,glyph,4,4,1,0xFFE0,vt::RGB565);vt::TextInkRect dirty{};vt::EndTextInkCapture(&dirty);
        const int rect[]{0,0,32,24};uintptr_t keyed=0x7F7BF4;
        bool stable=true,isolated=true,erased=true;
        for(int frame=0;frame<120;++frame) {
            const unsigned short scene=(frame&1) ? 0xFFFF : 0;
            std::fill(std::begin(screenPixels),std::end(screenPixels),scene);
            vt::Presentation32::TestGameCopy(screen,rect,cache,rect,&keyed,OfflineCopy);
            vt::PlaneOptions options;options.highResolution=true;
            vt::PixelPlane reference(32,24,options);unsigned short refNative[32*24]{};
            std::fill(std::begin(refNative),std::end(refNative),scene);
            reference.Paint(glyph,4,4,1,0xFFE0,{0,0,32,24},refNative,32,true);
            const unsigned int expected=reference.Composite(4,4,refNative[4*32+4]);
            void* upload=nullptr;int pitch=0;
            if(!vt::Presentation32::TestFrameUploadBegin(screen,&texture,&upload,&pitch)) return 2;
            for(int y=0;y<24;++y) memcpy((char*)upload+y*pitch,screenPixels+y*32,32*2);
            // Complete another movie frame while this texture still contains
            // the previous frame's native pixels. This is a deterministic
            // interleaving of the game and cnc-ddraw render threads.
            std::fill(std::begin(screenPixels),std::end(screenPixels),(unsigned short)(scene ? 0 : 0xFFFF));
            vt::Presentation32::TestGameCopy(screen,rect,cache,rect,&keyed,OfflineCopy);
            unsigned int before[64*48]{},after[64*48]{};
            vt::Presentation32::TestCpuTextOverlay(screen,before,64);
            vt::Presentation32::TestFrameUploadEnd(&texture);
            stable=stable && texture.pixels[4*32+4]==expected;
            vt::Presentation32::TestCpuTextOverlay(screen,after,64);
            isolated=isolated && !memcmp(before,after,sizeof(before));
        }
        Check(stable,"120 delayed movie uploads keep each frame's subtitle coverage and background together");
        Check(isolated,"old render uploads never erase the next frame's live HiDPI subtitle");
        ddsTable[32]=(void*)OfflineSurfaceUnlock;
        unsigned short refNative[32*24]{};
        std::fill(std::begin(refNative),std::end(refNative),(unsigned short)0xFFFF);
        vt::PixelPlane reference(32,24,{});
        reference.Paint(glyph,4,4,1,0xFFE0,{0,0,32,24},refNative,32,true);
        unlockExpected=refNative[4*32+4];
        std::fill(std::begin(screenPixels),std::end(screenPixels),(unsigned short)0xFFFF);
        vt::Presentation32::TestGameCopy(screen,rect,cache,rect,&keyed,OfflineCopyAndUnlock);
        Check(unlockObserved,"native subtitle markers finish before DDS Unlock wakes the renderer");
        // A caption change after staging must not change that staged frame.
        void* upload=nullptr;int pitch=0;
        vt::Presentation32::TestFrameUploadBegin(screen,&texture,&upload,&pitch);
        for(int y=0;y<24;++y) memcpy((char*)upload+y*pitch,screenPixels+y*32,32*2);
        memset(cachePixels,0,sizeof(cachePixels));vt::Presentation32::TestCpuTextTrack(cache);
        memset(screenPixels,0,sizeof(screenPixels));
        unsigned int cleared[64*48]{};
        vt::Presentation32::TestCpuTextOverlay(screen,cleared,64); // validate current erase
        vt::Presentation32::TestFrameUploadEnd(&texture);
        Check(texture.pixels[4*32+4]!=0xFF000000u,"caption end does not erase an already staged earlier frame");
        vt::Presentation32::TestFrameUploadBegin(screen,&texture,&upload,&pitch);
        memset(upload,0,32*24*2);vt::Presentation32::TestFrameUploadEnd(&texture);
        for(auto pixel:texture.pixels) erased=erased && pixel==0xFF000000u;
        Check(erased,"the next frame removes ended subtitles immediately without stale overlays");
        vt::SetPresentationWriter(nullptr);
        printf("Movie frames: %s\n",errors ? "FAIL" : "PASS");return errors ? 1 : 0;
    }
    FILE* report=fopen("cnc-qa.txt","w");if(report)fclose(report);
    if(argc>1 && !strcmp(argv[1],"--fractional-offline")) {
        // Compare the actual packed atlas and output vertices against an
        // independent contiguous source image, including tile seams and tails.
        for(int density : {2,5}) {
        vt::PlaneOptions options;options.highResolution=true;
        vt::PixelPlane plane(96,48,options);
        vt::GlyphRaster2 raster;raster.scale=density;raster.width=raster.rows=density;
        raster.coverage.resize(density*density);
        for(int i=0;i<density*density;++i) raster.coverage[i]=(unsigned char)(16+i*224/(density*density-1));
        vt::GlyphCell glyph{};glyph.inkRows=1;glyph.cov[0]=128;glyph.raster2=&raster;
        for(const auto& p:std::vector<std::array<int,3>>{{0,0,0xFFFF},{31,15,0xF800},
                {32,16,0x07E0},{63,31,0x001F},{64,32,0xFFFF},{95,47,0xFFFF},{48,23,0x07E0}})
            plane.Paint(glyph,p[0],p[1],1,(unsigned short)p[2],{0,0,96,48});
        std::vector<unsigned int> full(96*48*density*density);
        plane.OverlayRect(full.data(),96*density,{0,0,96,48},density);
        for(const auto& scales:std::vector<std::array<float,2>>{{1.01f,1.01f},{1.25f,1.25f},
                {4.0f/3,4.0f/3},{1.5f,1.5f},{1.75f,1.75f},{2,2},{2.25f,2.25f},{2.5f,2.5f},{3,3},
                {1.5f,1.25f},{2.4f,1.8f},{4.8f,3.6f},{2,3}}) {
            const float sx=scales[0],sy=scales[1];
            for(int cropped=0;cropped<2;++cropped) {
                const vt::PixelRect source=cropped ? vt::PixelRect{9,4,87,44} : vt::PixelRect{0,0,96,48};
                const float ox=cropped ? 37.25f : 0,oy=cropped ? 11.75f : 0;
                vt::Presentation32::TestOverlay frame;
                bool match=vt::Presentation32::TestBuildOverlay(plane,sx,sy,ox,oy,frame,&source);
                double largestError=0;
                for(int y=(int)ceil(oy-0.5);y<oy+(source.bottom-source.top)*sy-0.5;++y)
                    for(int x=(int)ceil(ox-0.5);x<ox+(source.right-source.left)*sx-0.5;++x)
                        for(int channel=0;channel<4;++channel) {
                            const double expected=SampleChannel(full,96*density,48*density,
                                source.left*density+(x-ox+0.5)*density/sx-0.5,
                                source.top*density+(y-oy+0.5)*density/sy-0.5,channel,frame.point);
                            const double actual=AtlasSample(frame,x,y,channel);
                            largestError=std::max(largestError,fabs(expected-actual));
                        }
                match=match && largestError<0.01;
                printf("fractional scale=%.4gx%.4g raster=%d cropped=%d max channel error=%.6f\n",sx,sy,density,cropped,largestError);
                Check(match,"packed atlas matches full-image sampling across tile seams and viewport offsets");
                Check(frame.point==(sx==density && sy==density),"point sampling requires exact raster density on both axes");
            }
        }
        vt::Presentation32::TestOverlay frame;
        Check(!vt::Presentation32::TestBuildOverlay(plane,1,1,0,0,frame),"native 1x retains the existing compositor");
        Check(!vt::Presentation32::TestBuildOverlay(plane,2,0.75f,0,0,frame),"downscaled axis retains existing fallback");
        Check(!vt::Presentation32::TestBuildOverlay(plane,std::numeric_limits<float>::infinity(),1.5f,0,0,frame),
            "nonfinite quad cannot allocate an overlay");
        plane.Clear({0,0,96,48});
        Check(vt::Presentation32::TestBuildOverlay(plane,1.5f,1.5f,0,0,frame) && frame.vertices.empty(),
            "erased text has no live draw cells or filter fringes");
        }
        printf("fractional overlay: %s\n",errors?"FAIL":"PASS");return errors?1:0;
    }
    if(argc>1 && !strcmp(argv[1],"--cpu-text")) {
        // Offline loading-surface regression: no window, GPU, cnc-ddraw or
        // Phobos is needed. Use the real registration/writer/erasure helpers.
        vt::Presentation32::TestCpuTextStart();
        unsigned char object[0x24]{};void* table[34]{};
        unsigned short memory[32*24]{};
        table[29]=(void*)CpuPitch;*(void***)object=table;
        ((int*)object)[1]=32;((int*)object)[2]=24;((int*)object)[4]=2;
        *(void**)(object+0x14)=memory;
        vt::GlyphCell glyph{};glyph.inkRows=1;glyph.cov[0]=128;
        vt::GlyphRaster2 hi;hi.width=2;hi.rows=2;hi.coverage={16,80,160,240};glyph.raster2=&hi;
        vt::Target target{memory,32,0,0,31,23};
        unsigned int first[64*48]{},again[64*48]{};
        vt::Presentation32::TestCpuTextTrack(object);
        vt::DrawCellAA(target,glyph,4,4,1,0xFFFF,vt::RGB565);
        Check(vt::Presentation32::Stats().glyphs==1,"CPU loading text retained before backend activation");
        Check(vt::Presentation32::TestCpuTextOverlay(object,first,64),"CPU loading sidecar available");
        bool high=true;
        for(int i=0;i<4;++i) high=high && (first[(8+i/2)*64+8+i%2]>>24)==hi.coverage[i];
        Check(high,"loading retains four independent output-grid coverages");
        for(int n=0;n<100;++n) {
            vt::Presentation32::TestCpuTextTrack(object);
            vt::DrawCellAA(target,glyph,4,4,1,0xFFFF,vt::RGB565);
        }
        vt::Presentation32::TestCpuTextOverlay(object,again,64);
        Check(!memcmp(first,again,sizeof(first)),"loading redraw does not darken or accumulate edges");
        unsigned char screen[0x24]{};void* screenTable[34]{},*ddsTable[34]{};
        unsigned short screenPixels[32*24]{};
        ddsTable[22]=(void*)OfflineDesc;OfflineDDS dds{ddsTable,screenPixels,32,24};
        screenTable[33]=(void*)0x4C1AB0;*(void***)screen=screenTable;
        *(void**)(screen+0x1C)=&dds;
        const int rect[]{0,0,32,24};uintptr_t opaque=0x7F7BC4;
        Check(vt::Presentation32::TestGameCopy(screen,rect,object,rect,&opaque,OfflineCopy),
            "loading BSurface copied through native software-copy hook");
        Check(vt::Presentation32::TestCpuTextOverlay(screen,again,64) && !memcmp(first,again,sizeof(first)),
            "BSurface to DSurface copy preserves independent 2x text");
        Check(!vt::Presentation32::TestGameCopy(screen,rect,object,rect,&opaque,FailedCopy),
            "failed loading copy returns failure");
        vt::Presentation32::TestCpuTextOverlay(screen,again,64);
        Check(!memcmp(first,again,sizeof(first)),"failed loading copy restores screen sidecar");
        memset(memory,0,sizeof(memory));
        vt::Presentation32::TestCpuTextOverlay(object,again,64);
        bool clear=true;for(auto p:again) clear=clear && !p;
        Check(clear,"native BSurface background restore erases low and high samples");
        vt::Presentation32::TestCpuTextRelease(object);
        Check(!vt::Presentation32::TestCpuTextOverlay(object,again,64),"BSurface destruction removes sidecar");
        vt::Presentation32::TestCpuTextTrack(object);
        vt::Presentation32::TestCpuTextOverlay(object,again,64);
        clear=true;for(auto p:again) clear=clear && !p;
        Check(clear,"reused BSurface object and allocation do not retain previous loading text");
        unsigned short replacement[32*24]{};
        *(void**)(object+0x14)=replacement;
        vt::Presentation32::TestCpuTextTrack(object);
        target.base=replacement;
        vt::DrawCellAA(target,glyph,4,4,1,0xFFFF,vt::RGB565);
        vt::Presentation32::TestCpuTextOverlay(object,again,64);
        Check(!memcmp(first,again,sizeof(first)),"BSurface buffer change creates a fresh sidecar");
        // The independent output-grid fringe can extend below the 1x raster.
        // It must be included even when its logical coverage is zero.
        memset(replacement,0,sizeof(replacement));
        vt::Presentation32::TestCpuTextTrack(object);
        glyph.cov[0]=0;hi.top=34;hi.left=-1;
        vt::BeginTextInkCapture();
        vt::DrawCellAA(target,glyph,4,0,1,0x07E0,vt::RGB565);
        vt::TextInkRect dirty{};
        Check(vt::EndTextInkCapture(&dirty) && dirty.left==3 && dirty.right==5 && dirty.top==17 && dirty.bottom==18,
            "subtitle capture includes HiDPI-only descender and negative bearing");
        for(int y=dirty.top;y<dirty.bottom;++y) for(int x=dirty.left;x<dirty.right;++x) replacement[y*32+x]=0;
        vt::Presentation32::TestCpuTextOverlay(object,again,64);
        clear=true;for(auto p:again) clear=clear && !p;
        Check(clear,"expanded subtitle erase removes HiDPI-only coloured fragments");
        Check(!vt::EndTextInkCapture(&dirty),"finished subtitle capture does not leak into another caption");
        hi.top=hi.left=0;hi.coverage={255,255,255,255};glyph.cov[0]=255;
        memset(replacement,0,sizeof(replacement));vt::Presentation32::TestCpuTextTrack(object);
        vt::BeginTextInkCapture(true);
        vt::DrawCellAA(target,glyph,4,4,1,0x001F,vt::RGB565);
        Check(vt::EndTextInkCapture(&dirty) && dirty.left==3 && dirty.top==3 && dirty.right==6 && dirty.bottom==6,
            "production CPU writer includes subtitle outline in erase bounds");
        uintptr_t keyed=0x7F7BF4;
        for(unsigned short scene : {static_cast<unsigned short>(0),static_cast<unsigned short>(0xFFFF),static_cast<unsigned short>(0)}) {
            std::fill(std::begin(screenPixels),std::end(screenPixels),scene);
            Check(vt::Presentation32::TestGameCopy(screen,rect,object,rect,&keyed,OfflineCopy),
                "production native keyed copy transfers cached subtitle outline");
            vt::Presentation32::TestCpuTextOverlay(screen,again,64);
            Check(again[8*64+6]==0xFF000000u && again[8*64+7]==0xFFFFFFFFu && again[8*64+8]==0xFF0000FFu,
                "production copy keeps white inner and black outer rims stable while retaining blue text");
            Check(screenPixels[4*32+3]==(scene ? 0xFFFE : 0xFFFF),"production copy resolves observable native outline marker");
            Check(!vt::Presentation32::TestGameCopy(screen,rect,object,rect,&opaque,FailedCopy),
                "failed outlined subtitle copy returns failure");
            vt::Presentation32::TestCpuTextOverlay(screen,first,64);
            Check(!memcmp(first,again,sizeof(first)),"failed subtitle copy restores adaptive edge and background metadata");
            std::fill(std::begin(screenPixels),std::end(screenPixels),scene);
            vt::Presentation32::TestCpuTextOverlay(screen,again,64);
            clear=true;for(auto p:again) clear=clear && !p;
            Check(clear,"bright and dark movie frame restores erase outlined subtitles without residue");
        }
        vt::Presentation32::TestCpuTextRelease(object);
        vt::SetPresentationWriter(nullptr);
        printf("CPU text: %s\n",errors?"FAIL":"PASS");return errors?1:0;
    }
    const bool perf2=argc>1 && !strcmp(argv[1],"--perf-2x");
    const bool fractional=argc>2 && !strcmp(argv[1],"--fractional");
    const float requestedScale=fractional ? (float)atof(argv[2]) : 1;
    const float requestedScaleY=fractional && argc>3 ? (float)atof(argv[3]) : requestedScale;
    const bool compat2=argc>1 && !strcmp(argv[1],"--compat-2x");
    const bool output2=argc>1 && (!strcmp(argv[1],"--2x") || compat2 || perf2);
    const bool fullscreen=argc>1 && !strcmp(argv[1],"--fullscreen");
    if(fullscreen || perf2 || fractional) SetProcessDPIAware();
    const int logicalW=fullscreen ? GetSystemMetrics(SM_CXSCREEN) : perf2 ? 1920 : 640;
    const int logicalH=fullscreen ? GetSystemMetrics(SM_CYSCREEN) : perf2 ? 1080 : 480;
    if(perf2) {
        char ini[MAX_PATH]{};GetModuleFileNameA(nullptr,ini,MAX_PATH);
        char* name=strrchr(ini,'\\');if(name) strcpy(name+1,"VectorText.ini");
        WritePrivateProfileStringA("VectorText","PresentProfile","1",ini);
    }
    vt::Log::Prepare();
    vt::Presentation32::PrepareEarly();
    if(argc>1 && !strcmp(argv[1],"--no-cnc")) {
        wchar_t path[MAX_PATH]{}; GetSystemDirectoryW(path,MAX_PATH);
        wcscat_s(path,L"\\ddraw.dll");
        HMODULE native=LoadLibraryW(path);
        using Create=HRESULT(WINAPI*)(GUID*,LPDIRECTDRAW*,IUnknown*);
        auto create=(Create)GetProcAddress(native,"DirectDrawCreate");
        LPDIRECTDRAW dd=nullptr;
        Check(create && SUCCEEDED(create(nullptr,&dd,nullptr)),"native DirectDrawCreate");
        unsigned short memory[32*32]{};
        vt::GlyphCell glyph{}; glyph.inkRows=1; glyph.cov[0]=128;
        vt::Target target{memory,32,0,0,31,31};
        vt::DrawCellAA(target,glyph,0,0,1,0xFFFF,vt::RGB565);
        Check(memory[0]!=0 && !vt::Presentation32::Stats().backend,"without cnc-ddraw native RGB565 path remains active");
        if(dd) dd->Release();
        vt::Log::Shutdown(); return errors?1:0;
    }
    WNDCLASSA wc{}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandle(nullptr); wc.lpszClassName="VT32Test";
    RegisterClassA(&wc);
    RECT desired{0,0,(LONG)(640*requestedScale),(LONG)(480*requestedScaleY)};
    AdjustWindowRect(&desired,WS_OVERLAPPEDWINDOW,FALSE);
    window=CreateWindowA(wc.lpszClassName,"VectorText presentation verification",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,fractional ? desired.right-desired.left : output2?1296:656,
        fractional ? desired.bottom-desired.top : output2?999:519,nullptr,nullptr,wc.hInstance,nullptr);
    ShowWindow(window,SW_SHOWNOACTIVATE);
    // Pixel checks must sample the renderer rather than an overlapping app.
    // Keep this short-lived QA window visible without taking keyboard focus.
    SetWindowPos(window,HWND_TOPMOST,80,80,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
    HMODULE module=LoadLibraryA("ddraw.dll");
    if(!module) return 2;
    using Create=HRESULT(WINAPI*)(GUID*,LPDIRECTDRAW*,IUnknown*);
    auto create=(Create)GetProcAddress(module,"DirectDrawCreate");
    LPDIRECTDRAW dd=nullptr;
    Check(create && SUCCEEDED(create(nullptr,&dd,nullptr)),"cnc-ddraw DirectDrawCreate");
    if(!dd) return 2;
    Check(SUCCEEDED(dd->SetCooperativeLevel(window,DDSCL_NORMAL)),"cooperative level");
    Check(SUCCEEDED(dd->SetDisplayMode(logicalW,logicalH,16)),"display mode RGB565");
    DDSURFACEDESC desc{}; desc.dwSize=sizeof(desc); desc.dwFlags=DDSD_CAPS;
    desc.ddsCaps.dwCaps=DDSCAPS_PRIMARYSURFACE;
    LPDIRECTDRAWSURFACE primary=nullptr;
    Check(SUCCEEDED(dd->CreateSurface(&desc,&primary,nullptr)),"primary surface");
    if(!primary) return 2;
    DDBLTFX fill{}; fill.dwSize=sizeof(fill);
    primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);
    Pump(800);
    auto stats=vt::Presentation32::Stats();
    printf("backend=%ld frames=%ld surfaces=%ld\n",stats.backend,stats.frames,stats.surfaces);
    const bool expectFallback=argc>1 && !strcmp(argv[1],"--fallback");
    Check(expectFallback ? !stats.backend : stats.backend>0,"presentation activation");
    if(fractional) {
        const int density=std::max(2,std::min(8,(int)ceil(std::max(requestedScale,requestedScaleY)-0.0001f)));
        vt::GlyphRaster2 hi;hi.scale=density;hi.width=hi.rows=16*density;hi.coverage.assign(hi.width*hi.rows,192);
        vt::GlyphCell mark{};mark.inkRows=16;mark.raster2=&hi;
        for(int y=0;y<16;++y) for(int x=0;x<16;++x) mark.cov[y*24+x]=128;
        DDSURFACEDESC d{};d.dwSize=sizeof(d);primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
        vt::Target t{(unsigned short*)d.lpSurface,d.lPitch/2,0,0,639,479};
        vt::DrawCellAA(t,mark,25,10,16,0xFFFF,vt::RGB565);primary->Unlock(nullptr);Pump(500);
        primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);primary->Unlock(nullptr);Pump(300);
        const bool enabled=vt::Cfg::HiDPI();
        const float actualScale=vt::Presentation32::TestObservedScale();
        const float actualScaleY=vt::Presentation32::TestObservedScale(true);
        printf("requested scale %.4gx%.4g, observed %.4gx%.4g\n",requestedScale,requestedScaleY,actualScale,actualScaleY);
        Check(enabled ? fabs(actualScale-requestedScale)<0.001f && fabs(actualScaleY-requestedScaleY)<0.001f
                      : actualScale==0 && actualScaleY==0,
            "fractional scale follows actual cnc-ddraw quad and respects HiDPI switch");
        Check(!enabled || (vt::OutputRasterScale()==density && vt::Presentation32::TestObservedRaster()==density),
            "actual output density selects an independent raster and a matching presenter");
        HDC dc=GetDC(window);
        const int ref=vt::Cfg::LinearBlend() ? (enabled ? 224 : 186) : (enabled ? 192 : 128);
        const int actual=GetRValue(GetPixel(dc,(int)ceil(32*requestedScale),(int)ceil(16*requestedScaleY)));
        printf("fractional tile-seam pixel=%d expected=%d\n",actual,ref);
        Check(abs(actual-ref)<=4,"fractional tile junction has continuous high-resolution coverage");
        ReleaseDC(window,dc);
        primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);Pump(300);
        dc=GetDC(window);
        Check(GetPixel(dc,(int)ceil(32*requestedScale),(int)ceil(16*requestedScaleY))==RGB(0,0,0),
            "fractional overlay and filter fringe disappear after text is erased");ReleaseDC(window,dc);
        // Caption gaps reset an empty CPU plane to density 2. Both rotating
        // upload textures must recover their high-resolution atlas afterwards,
        // including a real density change between successive captions.
        for(int captionDensity : {density,2,density}) {
            hi.scale=captionDensity;hi.width=hi.rows=16*captionDensity;
            hi.coverage.assign(hi.width*hi.rows,192);
            primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
            t.base=(unsigned short*)d.lpSurface;t.pitch=d.lPitch/2;
            vt::DrawCellAA(t,mark,25,10,16,0xFFFF,vt::RGB565);
            primary->Unlock(nullptr);Pump(300);
            bool stable=true;
            int minimum=255,maximum=0;
            for(int frame=0;frame<8;++frame) {
                primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
                primary->Unlock(nullptr);Pump(80);
                dc=GetDC(window);
                const int pixel=GetRValue(GetPixel(dc,(int)ceil(32*requestedScale),(int)ceil(16*requestedScaleY)));
                ReleaseDC(window,dc);
                minimum=std::min(minimum,pixel);maximum=std::max(maximum,pixel);
                stable=stable && abs(pixel-ref)<=4;
            }
            printf("returning caption raster=%d: screen range=%d..%d expected=%d\n",captionDensity,minimum,maximum,ref);
            Check(stable,"returning captions retain high-resolution coverage on every rotating texture");
            primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);Pump(300);
            dc=GetDC(window);
            Check(GetPixel(dc,(int)ceil(32*requestedScale),(int)ceil(16*requestedScaleY))==RGB(0,0,0),
                "caption gaps hide retained atlas contents");ReleaseDC(window,dc);
        }
        if(enabled && requestedScale>=2.4f) {
            hi.scale=density;hi.width=hi.rows=16*density;hi.coverage.assign(hi.width*hi.rows,255);
            for(int yy=0;yy<16;++yy) for(int xx=0;xx<16;++xx) mark.cov[yy*24+xx]=255;
            COLORREF rimOnBlack=0;int rimX=0;
            for(unsigned short scene : {static_cast<unsigned short>(0),static_cast<unsigned short>(0xFFFF)}) {
                DDBLTFX sceneFill{};sceneFill.dwSize=sizeof(sceneFill);sceneFill.dwFillColor=scene;
                primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&sceneFill);Pump(100);
                primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
                t.base=(unsigned short*)d.lpSurface;t.pitch=d.lPitch/2;
                vt::BeginTextInkCapture(true);
                vt::DrawCellAA(t,mark,25,10,16,0x001F,vt::RGB565);
                vt::TextInkRect ink{};vt::EndTextInkCapture(&ink);
                primary->Unlock(nullptr);Pump(300);
                primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);primary->Unlock(nullptr);Pump(120);
                dc=GetDC(window);
                const int sampleY=(int)ceil(16*requestedScaleY);
                if(!scene) {
                    const int boundary=(int)ceil(25*requestedScale);
                    for(int xx=boundary-3;xx<boundary;++xx) {
                        const COLORREF candidate=GetPixel(dc,xx,sampleY);
                        if(GetRValue(candidate)>GetRValue(rimOnBlack)) {rimOnBlack=candidate;rimX=xx;}
                    }
                    Check(GetRValue(rimOnBlack)>130 && GetGValue(rimOnBlack)>130,
                        "presenter dark caption has a visible white separating rim");
                } else {
                    const COLORREF rim=GetPixel(dc,rimX,sampleY);
                    Check(rim==rimOnBlack,"presenter caption rim retains its color across black and white movie frames");
                }
                Check(GetPixel(dc,(int)ceil(32*requestedScale),sampleY)==RGB(0,0,255),
                    "presenter stable subtitle rim preserves the blue body");
                ReleaseDC(window,dc);
            }
            primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);Pump(300);
        }
        if(enabled && requestedScale==1.5f && requestedScaleY==requestedScale) for(float next:{1.75f,2.0f,1.0f,1.5f}) {
            RECT size{0,0,(LONG)(640*next),(LONG)(480*next)};
            AdjustWindowRect(&size,WS_OVERLAPPEDWINDOW,FALSE);
            RECT position{};GetWindowRect(window,&position);
            OffsetRect(&size,position.left-size.left,position.top-size.top);
            SendMessageW(window,WM_ENTERSIZEMOVE,0,0);
            SendMessageW(window,WM_SIZING,WMSZ_BOTTOMRIGHT,(LPARAM)&size);
            // WM_SIZING only proposes a rectangle. A real OS sizing loop also
            // applies it before exiting, including the resulting WM_SIZE.
            SetWindowPos(window,nullptr,size.left,size.top,size.right-size.left,size.bottom-size.top,
                SWP_NOZORDER|SWP_NOACTIVATE);
            SendMessageW(window,WM_EXITSIZEMOVE,0,0);Pump(1000);
            primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);Pump(500);
            primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
            // cnc-ddraw can replace native backing memory on a live resize.
            t.base=(unsigned short*)d.lpSurface;t.pitch=d.lPitch/2;
            vt::DrawCellAA(t,mark,25,10,16,0xFFFF,vt::RGB565);primary->Unlock(nullptr);Pump(300);
            primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);primary->Unlock(nullptr);Pump(200);
            const float observed=vt::Presentation32::TestObservedScale();
            const int expected=next==1 ? 186 : 224;
            dc=GetDC(window);
            const int pixel=GetRValue(GetPixel(dc,(int)ceil(32*next),(int)ceil(16*next)));
            ReleaseDC(window,dc);
            printf("live resize %.4g: observed=%.4g pixel=%d expected=%d\n",next,observed,pixel,expected);
            Check(fabs(observed-(next==1 ? 0 : next))<0.001f && abs(pixel-expected)<=4,
                "live fractional/2x/1x transitions retain alignment and coverage");
            primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);Pump(100);
        }
        primary->Release();dd->RestoreDisplayMode();dd->Release();DestroyWindow(window);vt::Log::Shutdown();
        return errors?1:0;
    }
    if(perf2) {
        vt::GlyphCell glyph{};glyph.inkRows=16;
        vt::GlyphRaster2 hi;hi.width=32;hi.rows=32;hi.coverage.resize(1024);
        for(int y=0;y<16;++y) for(int x=0;x<16;++x) glyph.cov[y*24+x]=(unsigned char)(x*16+y);
        for(int y=0;y<32;++y) for(int x=0;x<32;++x) hi.coverage[y*32+x]=(unsigned char)(x*8+y/4);
        glyph.raster2=&hi;
        for(int frame=0;frame<120;++frame) {
            DDSURFACEDESC d{};d.dwSize=sizeof(d);primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
            memset(d.lpSurface,0,(size_t)d.lPitch*logicalH);
            vt::Target t{(unsigned short*)d.lpSurface,d.lPitch/2,0,0,logicalW-1,logicalH-1};
            for(int i=0;i<40;++i) vt::DrawCellAA(t,glyph,1200+i*16,2,16,0xF800,vt::RGB565);
            for(int row=0;row<6;++row) for(int i=0;i<40;++i)
                vt::DrawCellAA(t,glyph,292+i*16,797+row*19,16,0x07FF,vt::RGB565);
            primary->Unlock(nullptr);Pump(20);
        }
        const auto p=vt::Presentation32::Stats();
        char result[256];snprintf(result,sizeof(result),"PERF 1920x1080 -> 3840x2160: glyph %.3f ms/frame, upload %.3f ms/frame, overlay %.3f ms/frame; %ld uploads/%ld draws",
            p.glyphMs/120,p.uploadMs/std::max(1L,p.uploads),p.overlayMs/std::max(1L,p.overlays),p.uploads,p.overlays);
        Check(true,result);
        primary->Release();dd->RestoreDisplayMode();dd->Release();DestroyWindow(window);vt::Log::Shutdown();
        return errors?1:0;
    }
    if(fullscreen) {
        vt::GlyphRaster2 hi;hi.width=2;hi.rows=2;hi.coverage={255,0,0,255};
        vt::GlyphCell mark{};mark.inkRows=1;mark.cov[0]=128;mark.raster2=&hi;
        DDSURFACEDESC d{};d.dwSize=sizeof(d);primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
        vt::Target t{(unsigned short*)d.lpSurface,d.lPitch/2,0,0,logicalW-1,logicalH-1};
        vt::DrawCellAA(t,mark,20,20,1,0xFFFF,vt::RGB565);primary->Unlock(nullptr);Pump(500);
        HDC dc=GetDC(window);COLORREF pixel=GetPixel(dc,20,20);
        Check(GetRValue(pixel)>150 && GetRValue(pixel)<220,"native-resolution fullscreen retains 1x glyph coverage");ReleaseDC(window,dc);
        primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);Pump(300);
        dc=GetDC(window);Check(GetPixel(dc,20,20)==RGB(0,0,0),"fullscreen text erasure");ReleaseDC(window,dc);
        primary->Release();dd->RestoreDisplayMode();dd->Release();DestroyWindow(window);vt::Log::Shutdown();
        return errors?1:0;
    }
    if(output2) {
        vt::GlyphRaster2 hi;hi.width=2;hi.rows=2;hi.coverage={255,0,0,255};
        vt::GlyphCell mark{};mark.inkRows=1;mark.cov[0]=128;mark.raster2=&hi;
        DDSURFACEDESC d{};d.dwSize=sizeof(d);primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
        vt::Target t{(unsigned short*)d.lpSurface,d.lPitch/2,0,0,639,479};
        vt::DrawCellAA(t,mark,20,20,1,0xFFFF,vt::RGB565);primary->Unlock(nullptr);Pump(500);
        // First observed quad arms the overlay; later frames can use 2x glyph samples.
        primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);primary->Unlock(nullptr);Pump(300);
        HDC dc=GetDC(window);
        if(compat2) {
            Check(vt::Presentation32::TestPixel(20,20)==0xFFBABABA,"forced 1x at 2x output retains ordinary BGRA8 composition");
            const int actual=GetRValue(GetPixel(dc,40,40));
            Check(actual>0 && actual<230,"forced 1x text reaches the displayed scaled frame");ReleaseDC(window,dc);
            primary->Release();dd->RestoreDisplayMode();dd->Release();DestroyWindow(window);vt::Log::Shutdown();
            return errors?1:0;
        }
        printf("2x pixels: %06lx %06lx %06lx %06lx\n",GetPixel(dc,40,40),GetPixel(dc,41,40),GetPixel(dc,40,41),GetPixel(dc,41,41));
        Check(GetPixel(dc,40,40)==RGB(255,255,255) && GetPixel(dc,41,40)==RGB(0,0,0) &&
            GetPixel(dc,40,41)==RGB(0,0,0) && GetPixel(dc,41,41)==RGB(255,255,255),
            "2x output retains four independent glyph samples, not enlarged 1x coverage");
        ReleaseDC(window,dc);
        // Check real partial alpha over a non-black background, not just opaque
        // samples: sRGB state must preserve the production linear compositor.
        fill.dwFillColor=0x4208;
        primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);
        vt::GlyphRaster2 gray;gray.width=2;gray.rows=2;gray.coverage={32,96,160,224};
        mark.raster2=&gray;
        primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
        vt::DrawCellAA(t,mark,20,20,1,0xFFFF,vt::RGB565);primary->Unlock(nullptr);Pump(300);
        dc=GetDC(window);
        bool grayMatches=true;
        for(int i=0;i<4;++i) {
            vt::GlyphCell sample{};sample.inkRows=1;sample.cov[0]=gray.coverage[i];
            vt::PixelPlane expected(1,1,{});expected.Paint(sample,0,0,1,0xFFFF,{0,0,1,1});
            const int ref=(expected.Composite(0,0,0x4208)>>16)&255;
            const int actual=GetRValue(GetPixel(dc,40+i%2,40+i/2));
            printf("2x linear gray: %d reference=%d\n",actual,ref);
            if(abs(actual-ref)>2) grayMatches=false;
        }
        Check(grayMatches,"2x partial alpha matches linear blending over a non-black background");ReleaseDC(window,dc);
        mark.raster2=&hi;fill.dwFillColor=0;
        primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);
        DDSURFACEDESC cacheDesc{};cacheDesc.dwSize=sizeof(cacheDesc);
        cacheDesc.dwFlags=DDSD_CAPS|DDSD_WIDTH|DDSD_HEIGHT;
        cacheDesc.dwWidth=4;cacheDesc.dwHeight=4;cacheDesc.ddsCaps.dwCaps=DDSCAPS_OFFSCREENPLAIN;
        LPDIRECTDRAWSURFACE cache=nullptr;
        Check(SUCCEEDED(dd->CreateSurface(&cacheDesc,&cache,nullptr)),"2x cached text surface");
        if(cache) {
            cache->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);
            DDSURFACEDESC cd{};cd.dwSize=sizeof(cd);cache->Lock(nullptr,&cd,DDLOCK_WAIT,nullptr);
            vt::Target ct{(unsigned short*)cd.lpSurface,cd.lPitch/2,0,0,3,3};
            vt::DrawCellAA(ct,mark,0,0,1,0xFFFF,vt::RGB565);cache->Unlock(nullptr);
            RECT src{0,0,1,1};primary->BltFast(20,20,cache,&src,DDBLTFAST_WAIT);Pump(300);
            dc=GetDC(window);
            Check(GetPixel(dc,40,40)==RGB(255,255,255) && GetPixel(dc,41,40)==RGB(0,0,0) &&
                GetPixel(dc,41,41)==RGB(255,255,255),"2x cached text transfers all four samples to the primary");ReleaseDC(window,dc);
            cache->Release();
        }
        primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);Pump(300);
        dc=GetDC(window);Check(GetPixel(dc,40,40)==RGB(0,0,0) && GetPixel(dc,41,41)==RGB(0,0,0),
            "2x erase clears old overlay tiles");ReleaseDC(window,dc);
        // Native green health bars and green vector text use different layers.
        // Repeated black/shroud restores must erase both, including old GPU
        // tiles from alternating upload textures with the bicubic world shader.
        bool barsClean=true;
        for(int frame=0;frame<30;++frame) {
            const int x=80+frame*5,y=80+frame*2;
            primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
            auto* base=(unsigned short*)d.lpSurface;
            memset(base,0,(size_t)d.lPitch*480);
            for(int i=0;i<16;++i) base[y*(d.lPitch/2)+x+i]=0x07E0;
            vt::DrawCellAA(t,mark,x,y+8,1,0x07E0,vt::RGB565);
            primary->Unlock(nullptr);Pump(35);
            primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
            memset(d.lpSurface,0,(size_t)d.lPitch*480);
            primary->Unlock(nullptr);Pump(35);
            dc=GetDC(window);
            barsClean=barsClean && vt::Presentation32::TestPixel(x+8,y)==0xFF000000 &&
                GetPixel(dc,(x+8)*2,y*2)==RGB(0,0,0) && GetPixel(dc,x*2,(y+8)*2)==RGB(0,0,0);
            ReleaseDC(window,dc);
        }
        Check(barsClean,"2x moving native green bars and text leave no trails after black CPU restores");
        primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
        for(int i=0;i<200;++i)
            vt::DrawCellAA(t,mark,(i%20)*32,120+(i/20)*32,1,0xFFFF,vt::RGB565);
        primary->Unlock(nullptr);Pump(300);
        dc=GetDC(window);bool atlasMapped=true;
        for(int i:{0,15,16,31,159,199}) {
            const int x=(i%20)*64,y=240+(i/20)*64;
            atlasMapped=atlasMapped && GetPixel(dc,x,y)==RGB(255,255,255) && GetPixel(dc,x+1,y)==RGB(0,0,0);
        }
        Check(atlasMapped,"grown sparse atlas maps first, middle and last cells to original screen coordinates");ReleaseDC(window,dc);
        primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);
        primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
        vt::DrawCellAA(t,mark,20,20,1,0xFFFF,vt::RGB565);primary->Unlock(nullptr);Pump(300);
        dc=GetDC(window);
        Check(GetPixel(dc,40,40)==RGB(255,255,255) && GetPixel(dc,1216,816)==RGB(0,0,0),
            "repacking a shrunken atlas does not draw abandoned old cells");ReleaseDC(window,dc);
        primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);Pump(100);
        // Resize the effective cnc-ddraw viewport through its normal messages.
        RECT size{0,0,640,480};AdjustWindowRect(&size,WS_OVERLAPPEDWINDOW,FALSE);
        SendMessageW(window,WM_ENTERSIZEMOVE,0,0);
        SendMessageW(window,WM_SIZING,WMSZ_BOTTOMRIGHT,(LPARAM)&size);
        SendMessageW(window,WM_EXITSIZEMOVE,0,0);
        Pump(500);
        primary->Lock(nullptr,&d,DDLOCK_WAIT,nullptr);
        vt::DrawCellAA(t,mark,20,20,1,0xFFFF,vt::RGB565);primary->Unlock(nullptr);Pump(500);
        dc=GetDC(window);COLORREF one=GetPixel(dc,20,20);
        printf("resized 1x sample: %06lx\n",one);
        Check(GetRValue(one)>150 && GetRValue(one)<220,"2x to 1x resize returns to ordinary BGRA8 coverage");ReleaseDC(window,dc);
        primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);Pump(200);
        dc=GetDC(window);Check(GetPixel(dc,20,20)==RGB(0,0,0),"1x resize retains ordinary erasure");ReleaseDC(window,dc);
        primary->Release();dd->RestoreDisplayMode();dd->Release();DestroyWindow(window);vt::Log::Shutdown();
        return errors?1:0;
    }
    DDSURFACEDESC locked{}; locked.dwSize=sizeof(locked);
    Check(SUCCEEDED(primary->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr)),"lock");
    vt::GlyphCell glyph{}; glyph.inkRows=16;
    for(int y=0;y<16;++y) for(int x=0;x<16;++x) {
        glyph.cov[y*24+x]=(unsigned char)(y*16+x);
        if(y*16+x>=128) glyph.bits[y*3+(x>>3)]|=0x80>>(x&7);
    }
    vt::Target target{(unsigned short*)locked.lpSurface,locked.lPitch/2,0,0,639,479};
    vt::DrawCellAA(target,glyph,32,32,16,0xFFFF,vt::RGB565);
    const unsigned short nativeSample=target.base[33*target.pitch+32];
    Check(nativeSample!=0,"native compatibility pixels support ordinary CPU drawing paths");
    primary->Unlock(nullptr);
    Pump(400);
    if(!expectFallback) {
        Check(vt::Presentation32::TestPixel(32,33)!=0xFF000000,"coverage reaches BGRA8 output");
        Check(vt::Presentation32::TestPixel(47,47)==0xFFFFFFFF,"opaque glyph reaches output");
        HDC dc=GetDC(window);
        const COLORREF sampledOpaque=GetPixel(dc,47,47);
        Check(sampledOpaque==RGB(255,255,255),"opaque glyph reaches the actual displayed frame");
        COLORREF middle=GetPixel(dc,40,40);
        unsigned int composed=vt::Presentation32::TestPixel(40,40);
        if(sampledOpaque!=RGB(255,255,255) || middle!=RGB((composed>>16)&255,(composed>>8)&255,composed&255)) {
            POINT sample{47,47}; ClientToScreen(window,&sample);
            printf("display sampling: opaque=0x%08X middle=0x%08X expected=0x%08X at=(%ld,%ld) visible=%d covered=%d\n",
                sampledOpaque,middle,composed,sample.x,sample.y,IsWindowVisible(window),WindowFromPoint(sample)!=window);
        }
        Check(middle==RGB((composed>>16)&255,(composed>>8)&255,composed&255),
              "partial coverage reaches the actual displayed frame without 565 quantization");
        ReleaseDC(window,dc);
        unsigned char gameSurface[0x24]{};
        *(LPDIRECTDRAWSURFACE*)(gameSurface+0x1C)=primary;
        const int clip[]{40,40,8,8}, rect[]{32,32,16,16};
        unsigned int outside=vt::Presentation32::TestPixel(32,33);
        vt::Presentation32::TestGameFill(gameSurface,clip,rect,0,(vt::Presentation32::TestFill)CpuFill);
        Pump(150);
        Check(vt::Presentation32::TestPixel(47,47)==0xFF000000,
              "game CPU fill clears retained text even when RGB565 pixels stay identical");
        Check(vt::Presentation32::TestPixel(32,33)==outside,
              "game CPU fill respects the clip rectangle");
        // This deliberately bypasses our game FillRect hook, as SHP rasterisers
        // and other direct memory writes do in the real game.
        locked.dwSize=sizeof(locked); primary->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
        vt::DrawCellAA(target,glyph,32,32,16,0xFFFF,vt::RGB565);
        primary->Unlock(nullptr); Pump(150);
        Check(vt::Presentation32::TestPixel(47,47)==0xFFFFFFFF,"unhooked erase starts with visible ink");
        CpuFill(gameSurface,nullptr,clip,rect,0); Pump(150);
        Check(vt::Presentation32::TestPixel(47,47)==0xFF000000,
              "unhooked CPU background restore removes retained text");
        // Same-valued fills must erase the text layer, even though 565 is unchanged.
        primary->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);
        Pump(250);
        Check(vt::Presentation32::TestPixel(47,47)==0xFF000000,"identical background fill erases text");
        // Cached offscreen text must survive a copy into the primary.
        DDSURFACEDESC cached{}; cached.dwSize=sizeof(cached);
        cached.dwFlags=DDSD_CAPS|DDSD_WIDTH|DDSD_HEIGHT;
        cached.dwWidth=64; cached.dwHeight=64; cached.ddsCaps.dwCaps=DDSCAPS_OFFSCREENPLAIN|DDSCAPS_SYSTEMMEMORY;
        LPDIRECTDRAWSURFACE source=nullptr;
        Check(SUCCEEDED(dd->CreateSurface(&cached,&source,nullptr)),"cached surface");
        source->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);
        locked.dwSize=sizeof(locked); source->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
        vt::Target st{(unsigned short*)locked.lpSurface,locked.lPitch/2,0,0,63,63};
        vt::DrawCellAA(st,glyph,0,0,16,0xFFFF,vt::RGB565); source->Unlock(nullptr);
        RECT sr{0,0,16,16};
        primary->BltFast(80,80,source,&sr,DDBLTFAST_WAIT);
        Pump(250);
        Check(vt::Presentation32::TestPixel(95,95)==0xFFFFFFFF,"cached text follows BltFast");
        DDCOLORKEY key{}; source->SetColorKey(DDCKEY_SRCBLT,&key);
        primary->BltFast(120,80,source,&sr,DDBLTFAST_WAIT|DDBLTFAST_SRCCOLORKEY);
        Pump(250);
        Check(vt::Presentation32::TestPixel(135,95)==0xFFFFFFFF,"color-keyed copy retains cached text");
        locked.dwSize=sizeof(locked); primary->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
        ((unsigned short*)locked.lpSurface)[95*(locked.lPitch/2)+135]=0xF800;
        primary->Unlock(nullptr); Pump(250);
        Check(vt::Presentation32::TestPixel(135,95)==0xFFFF0000,"native CPU overwrite invalidates old text");
        locked.dwSize=sizeof(locked); primary->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
        vt::Target pt{(unsigned short*)locked.lpSurface,locked.lPitch/2,0,0,639,479};
        vt::DrawCellAA(pt,glyph,160,80,16,0xFFFF,vt::RGB565);
        primary->Unlock(nullptr); Pump(150);
        locked.dwSize=sizeof(locked); primary->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
        ((unsigned short*)locked.lpSurface)[95*(locked.lPitch/2)+175]=0x001F;
        vt::DrawCellAA(pt,glyph,160,80,16,0xFFFF,vt::RGB565);
        primary->Unlock(nullptr); Pump(150);
        Check(vt::Presentation32::TestPixel(175,95)==0xFFFFFFFF,"identical glyph repainted over changed background survives");
        unsigned int prior=vt::Presentation32::TestPixel(161,80);
        locked.dwSize=sizeof(locked); primary->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
        vt::DrawCellAA(pt,glyph,160,80,16,0xFFFF,vt::RGB565);
        primary->Unlock(nullptr); Pump(150);
        Check(vt::Presentation32::TestPixel(161,80)==prior,"repeated drawing retains the same edge weight");
        // Funds are centred and change shape/length. The HUD restores the
        // same-valued background through XSurface's CPU copy, not DDS::Blt.
        unsigned char counterSurface[0x24]{},backgroundSurface[0x24]{};
        void* counterTable[34]{},*backgroundTable[34]{};
        GameSurface(counterSurface,counterTable,primary);
        GameSurface(backgroundSurface,backgroundTable,source);
        source->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);
        wchar_t fontPath[MAX_PATH]{}; GetWindowsDirectoryW(fontPath,MAX_PATH);
        wcscat_s(fontPath,L"\\Fonts\\arial.ttf");
        char latinPath[MAX_PATH]{}; WideCharToMultiByte(CP_UTF8,0,fontPath,-1,latinPath,MAX_PATH,nullptr,nullptr);
        vt::GlyphSource font;
        Check(font.Init(latinPath,13,400,3,16,13),"counter font"); font.SetAntiAlias(true);
        uintptr_t opaque=0x7F7BC4;
        const int counterRect[]{80,96,64,24},backgroundRect[]{0,32,64,24};
        bool clean=true;
        for(const char* value : {"88928","1111","0"}) {
            CpuCopy(counterSurface,counterRect,backgroundSurface,backgroundRect,&opaque,0,3,1000,0);
            vt::PixelPlane expected(640,480,{});
            Counter(primary,font,value,expected); Pump(150);
            for(int y=96;y<120;++y) for(int x=80;x<144;++x)
                clean=clean && vt::Presentation32::TestPixel(x,y)==expected.Composite(x,y,0);
        }
        Check(clean,"unhooked CPU restores leave no old strokes as centred digit count decreases");
        // Memory-backed XSurface sources have an allocator at +1C rather
        // than a DDS interface, and zero-keyed copies leave transparent ink.
        unsigned char memorySurface[0x24]{}; void* memoryTable[34]{};
        unsigned short background[64*24]{};
        memoryTable[29]=(void*)CpuPitch;
        *(void***)memorySurface=memoryTable;
        ((int*)memorySurface)[1]=64; ((int*)memorySurface)[2]=24; ((int*)memorySurface)[4]=2;
        *(void**)(memorySurface+0x14)=background;
        uintptr_t keyed=0x7F7BF4;
        const int memoryRect[]{0,0,64,24};
        int oldX=0,oldY=0; unsigned int old=0xFF000000;
        for(int y=96;y<120 && old==0xFF000000;++y) for(int x=80;x<144;++x) {
            const auto pixel=vt::Presentation32::TestPixel(x,y);
            if(pixel!=0xFF000000) { oldX=x; oldY=y; old=pixel; break; }
        }
        Check(old!=0xFF000000,"transparent/rollback checks start with visible counter ink");
        vt::Presentation32::TestGameCopy(counterSurface,counterRect,memorySurface,memoryRect,&keyed,CpuCopy);
        Pump(150);
        Check(vt::Presentation32::TestPixel(oldX,oldY)==old,"zero-keyed memory source preserves destination text");
        Check(!vt::Presentation32::TestGameCopy(counterSurface,counterRect,memorySurface,memoryRect,&opaque,FailedCopy),
              "CPU copy failure is propagated"); Pump(150);
        Check(vt::Presentation32::TestPixel(oldX,oldY)==old,"failed CPU copy restores the retained text layer");
        const int clippedDest[]{78,96,64,24},clippedSource[]{-2,0,64,24};
        vt::Presentation32::TestGameCopy(counterSurface,clippedDest,memorySurface,clippedSource,&opaque,CpuCopy);
        Pump(150);
        bool erased=true;
        for(int y=96;y<120;++y) for(int x=80;x<142;++x)
            erased=erased && vt::Presentation32::TestPixel(x,y)==0xFF000000;
        Check(erased,"clipped opaque memory background restore clears text without moving the source offset");
        Check(vt::Presentation32::TestPixel(175,95)==0xFFFFFFFF,"CPU background restore preserves text outside its rectangle");
        // The source bytes change here. Retained source text must survive
        // the native CPU copy's internal Lock/Unlock invalidation checks.
        DDBLTFX red{}; red.dwSize=sizeof(red); red.dwFillColor=0xF800;
        source->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&red);
        locked.dwSize=sizeof(locked); source->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
        st.base=(unsigned short*)locked.lpSurface; st.pitch=locked.lPitch/2;
        vt::DrawCellAA(st,glyph,0,0,16,0xFFFF,vt::RGB565); source->Unlock(nullptr);
        const int transferred[]{200,200,16,16},inkRect[]{0,0,16,16};
        vt::Presentation32::TestGameCopy(counterSurface,transferred,backgroundSurface,inkRect,&opaque,CpuCopy);
        Pump(150);
        vt::PixelPlane transferredExpected(16,16,{});
        transferredExpected.Paint(glyph,0,0,16,0xFFFF,{0,0,16,16});
        bool transferredClean=true;
        for(int y=0;y<16;++y) for(int x=0;x<16;++x)
            transferredClean=transferredClean && vt::Presentation32::TestPixel(200+x,200+y)==
                transferredExpected.Composite(x,y,0xF800);
        Check(transferredClean,"CPU copy transfers cached coverage even when native background bytes change");
        source->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);
        locked.dwSize=sizeof(locked); source->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
        vt::DrawCellAA(st,glyph,0,0,16,0xFFFF,vt::RGB565); source->Unlock(nullptr);
        DDBLTFX blue{}; blue.dwSize=sizeof(blue); blue.dwFillColor=0x001F;
        RECT transferRect{200,200,216,216};
        primary->Blt(&transferRect,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&blue);
        vt::Presentation32::TestGameCopy(counterSurface,transferred,backgroundSurface,inkRect,&keyed,CpuCopy);
        Pump(150); transferredClean=true;
        for(int y=0;y<16;++y) for(int x=0;x<16;++x)
            transferredClean=transferredClean && vt::Presentation32::TestPixel(200+x,200+y)==
                transferredExpected.Composite(x,y,0x001F);
        Check(transferredClean,"zero-keyed CPU copy transfers cached coverage over unchanged destination background");
        // A shroud/SHP-style raster write does not call our Fill/Copy hooks.
        // Restore black, keep a small visible world region, move red text, and
        // check the whole resulting frame after many updates.
        DWORD stressStart=GetTickCount();
        int redX=0,redY=0;
        for(int frame=0;frame<1000;++frame) {
            locked.dwSize=sizeof(locked); primary->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
            auto pixels=(unsigned short*)locked.lpSurface; int pitch=locked.lPitch/2;
            for(int y=0;y<480;++y) memset(pixels+y*pitch,0,640*2);
            for(int y=240;y<300;++y) for(int x=100;x<200;++x) pixels[y*pitch+x]=0x07E0;
            redX=200+frame%400; redY=20+frame%180;
            vt::Target frameTarget{pixels,pitch,0,0,639,479};
            vt::DrawCellAA(frameTarget,glyph,redX,redY,16,0xF800,vt::RGB565);
            primary->Unlock(nullptr);
        }
        Pump(150);
        vt::PixelPlane stressExpected(640,480,{});
        stressExpected.Paint(glyph,redX,redY,16,0xF800,{0,0,640,480});
        bool noTrails=true;
        for(int y=0;y<480;++y) for(int x=0;x<640;++x) {
            unsigned short bg=x>=100 && x<200 && y>=240 && y<300 ? 0x07E0 : 0;
            noTrails=noTrails && vt::Presentation32::TestPixel(x,y)==stressExpected.Composite(x,y,bg);
        }
        Check(noTrails,"1000 unhooked black-shroud updates preserve world pixels and leave no red-text trails");
        printf("shroud stress: 1000 updates in %lu ms\n",GetTickCount()-stressStart);
        source->Release();
        primary->Release(); primary=nullptr;
        // A flipping chain trades storage pointers, not the text layers.
        desc.dwFlags=DDSD_CAPS|DDSD_BACKBUFFERCOUNT; desc.dwBackBufferCount=1;
        desc.ddsCaps.dwCaps=DDSCAPS_PRIMARYSURFACE|DDSCAPS_FLIP|DDSCAPS_COMPLEX;
        Check(SUCCEEDED(dd->CreateSurface(&desc,&primary,nullptr)),"flipping primary");
        DDSCAPS caps{}; caps.dwCaps=DDSCAPS_BACKBUFFER;
        LPDIRECTDRAWSURFACE back=nullptr;
        Check(SUCCEEDED(primary->GetAttachedSurface(&caps,&back)),"backbuffer discovery");
        if(back) {
            back->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);
            locked.dwSize=sizeof(locked); back->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
            vt::Target bt{(unsigned short*)locked.lpSurface,locked.lPitch/2,0,0,639,479};
            vt::DrawCellAA(bt,glyph,200,80,16,0xFFFF,vt::RGB565); back->Unlock(nullptr);
            Check(SUCCEEDED(primary->Flip(nullptr,DDFLIP_WAIT)),"flip");
            Pump(250);
            Check(vt::Presentation32::TestPixel(215,95)==0xFFFFFFFF,"text follows flipped storage");
            // The now-visible primary must preserve the text in a locked idle frame.
            locked.dwSize=sizeof(locked); primary->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
            primary->Unlock(nullptr); Pump(150);
            Check(vt::Presentation32::TestPixel(215,95)==0xFFFFFFFF,"idle lock preserves flipped text");
            back->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fill);
            primary->Flip(nullptr,DDFLIP_WAIT); Pump(250);
            Check(vt::Presentation32::TestPixel(215,95)==0xFF000000,"next cleared flip has no text ghost");
            back->Release();
        }
    }
    stats=vt::Presentation32::Stats();
    printf("backend=%ld frames=%ld glyphs=%ld errors=%d\n",stats.backend,stats.frames,stats.glyphs,errors);
    primary->Release(); dd->RestoreDisplayMode(); dd->Release();
    Check(vt::Presentation32::Stats().surfaces==0,"surface release frees all tracked surfaces including attached buffers");
    DestroyWindow(window); vt::Log::Shutdown();
    return errors?1:0;
}
