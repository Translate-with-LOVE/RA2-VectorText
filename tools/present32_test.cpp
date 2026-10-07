// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "PixelPlane.h"
#include "PixelWriter.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
#include <chrono>

static int failures;
#define CHECK(c, text) do { if (!(c)) { ++failures; printf("FAIL: %s\n", text); } } while (0)

int main()
{
    const int w = 96, h = 48;
    vt::PlaneOptions options;
    vt::PixelPlane plane(w, h, options);
    std::vector<unsigned short> background(w * h, 0);
    const auto original = background;
    vt::GlyphCell glyph{};
    glyph.width = 24; glyph.inkRows = 16;
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x)
        glyph.cov[y * 24 + x] = (unsigned char)(y * 16 + x);
    plane.Paint(glyph, 3, 4, 16, 0xFFFF, { 0, 0, w, h });
    CHECK(background == original, "painting writes no RGB565 text pixels");
    CHECK(plane.TileCount() == 2, "text allocates sparse tiles only");
    std::set<uint32_t> fullColor, quantized;
    vt::SetLinearBlend(true); vt::SetDither(false); vt::SetCoverageGamma(1.0);
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x)
    {
        fullColor.insert(plane.Composite(x + 3, y + 4, 0));
        quantized.insert(vt::PixelPlane::Expand565(vt::Blend(0, 0xFFFF, y * 16 + x, vt::RGB565)));
    }
    printf("coverage levels: BGRA8=%zu RGB565=%zu\n", fullColor.size(), quantized.size());
    CHECK(fullColor.size() > quantized.size(), "direct 32-bit coverage retains more tonal levels");
    CHECK(plane.Composite(3, 4, 0x07FF) == vt::PixelPlane::Expand565(0x07FF), "zero coverage preserves background");
    CHECK(plane.Composite(18, 19, 0) == 0xFFFFFFFFu, "opaque foreground preserved exactly");
    const uint32_t edge = plane.Composite(4, 4, 0);
    plane.BeginWrite();
    plane.Paint(glyph, 3, 4, 16, 0xFFFF, {0,0,w,h});
    CHECK(plane.Composite(4,4,0)==edge,"new-lock repaint preserves partial coverage instead of accumulating weight");
    vt::PixelPlane copied(w, h, options);
    copied.Copy(&plane, { 0, 0, w, h }, { 0, 0, w, h }, background.data(), w);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
        CHECK(copied.Composite(x, y, 0) == plane.Composite(x, y, 0), "surface copy preserves text layer");
    copied.Copy(&plane, { 0, 0, 32, 24 }, { 32, 0, 64, 24 }, background.data(), w,
                nullptr, 0, true, 0, 0);
    CHECK(copied.At(50, 19).a == 65535, "colour-keyed cached text survives even without native text pixels");
    copied.Clear({ 32, 0, 64, 24 });
    CHECK(!copied.At(50, 19).a && copied.At(18, 19).a, "clearing a background erases only its covered text");
    copied.Copy(&copied, { 0, 0, 32, 24 }, { 8, 0, 40, 24 }, background.data(), w);
    CHECK(copied.At(26, 19).a == 65535, "overlapping self-blit snapshots source");
    vt::PixelPlane scaled(w, h, options);
    scaled.Copy(&plane, { 0, 0, 32, 24 }, { 0, 0, 64, 48 }, background.data(), w);
    CHECK(scaled.At(36, 38).a == 65535, "stretched surface tracks text coordinates");
    // Compare the sparse copy with independent nearest-neighbour sampling,
    // including mirrored tiles and a destination clipped on all four edges.
    for (int flags=0;flags<4;++flags) {
        vt::PixelPlane mirror(w,h,options);
        const vt::PixelRect sr{0,0,64,32}, dr{-7,-5,100,52};
        mirror.Copy(&plane,sr,dr,background.data(),w,nullptr,0,
                    false,0,0,false,0,0,(flags&1)!=0,(flags&2)!=0);
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            int sx=(x-dr.left)*64/(dr.right-dr.left),sy=(y-dr.top)*32/(dr.bottom-dr.top);
            if(flags&1) sx=63-sx; if(flags&2) sy=31-sy;
            const auto expected=plane.At(sx,sy),actual=mirror.At(x,y);
            CHECK(actual.r==expected.r && actual.g==expected.g && actual.b==expected.b &&
                  actual.a==expected.a,"sparse mirrored/clipped copy agrees with scalar reference");
        }
    }
    std::vector<uint32_t> out(w * h, 0x12345678);
    plane.CompositeRect(background.data(), w, out.data(), w, { 0, 0, w, h });
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
        CHECK(out[y * w + x] == plane.Composite(x, y, 0), "tiled full-frame compositor matches reference");
    plane.Clear({ 0, 0, w, h });
    CHECK(plane.Empty() && plane.TileCount() == 0, "empty frame frees text tiles");
    vt::PixelPlane clipped(w, h, options);
    glyph.inkX = -2; glyph.inkY = -3;
    clipped.Paint(glyph, 0, 0, 16, 0xF800, { 2, 2, 8, 8 });
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
        CHECK(!clipped.At(x, y).a || (x >= 2 && x < 8 && y >= 2 && y < 8), "glyph bearings respect surface clip");
    // Native compatibility pixels make same-colour opaque CPU overwrites
    // observable. The final compositor must ignore their quantized colour.
    vt::PixelPlane compat(w,h,options),reference(w,h,options);
    glyph.inkX=glyph.inkY=0;
    std::fill(background.begin(),background.end(),(unsigned short)0x001F);
    compat.Paint(glyph,3,4,16,0x07FF,{0,0,w,h},background.data(),w);
    reference.Paint(glyph,3,4,16,0x07FF,{0,0,w,h});
    compat.ValidateNative(background.data(),w);
    bool exact=true;
    for(int y=0;y<h;++y) for(int x=0;x<w;++x)
        exact=exact && compat.Composite(x,y,background[y*w+x])==reference.Composite(x,y,0x001F);
    CHECK(exact,"native proxy is excluded from final BGRA8 blend at every coverage level");
    std::fill(background.begin(),background.end(),(unsigned short)0x001F);
    compat.ValidateNative(background.data(),w);
    CHECK(compat.Empty(),"same original background restored through unhooked CPU writes erases text");
    bool bounded=true;
    for(int frame=0;frame<1000;++frame) {
        std::fill(background.begin(),background.end(),(unsigned short)0);
        compat.ValidateNative(background.data(),w); compat.BeginWrite();
        compat.Paint(glyph,frame%(w-24),frame%(h-16),16,0xF800,{0,0,w,h},background.data(),w);
        bounded=bounded && compat.TileCount()<=4;
    }
    CHECK(bounded,"1000 moving red-text/black-shroud restores do not accumulate stale tiles");
    std::fill(background.begin(),background.end(),(unsigned short)0);
    compat.ValidateNative(background.data(),w);
    compat.Paint(glyph,3,4,16,0,{0,0,w,h},background.data(),w);
    CHECK(!compat.Empty(),"black shadow on black is tracked without visible final colour");
    std::fill(background.begin(),background.end(),(unsigned short)0); compat.ValidateNative(background.data(),w);
    CHECK(compat.Empty(),"restoring black also invalidates black shadow coverage");
    vt::PlaneOptions highOptions=options;highOptions.highResolution=true;
    vt::PixelPlane highPlane(w,h,highOptions),lowPlane(w,h,options);
    vt::GlyphRaster2 raster; raster.width=2;raster.rows=2;raster.coverage={255,0,128,255};
    vt::GlyphCell highGlyph{};highGlyph.inkRows=1;highGlyph.cov[0]=128;highGlyph.raster2=&raster;
    std::fill(background.begin(),background.end(),(unsigned short)0);
    highPlane.Paint(highGlyph,10,10,1,0xFFFF,{0,0,w,h},background.data(),w);
    lowPlane.Paint(highGlyph,10,10,1,0xFFFF,{0,0,w,h});
    CHECK(highPlane.Composite(10,10,0)==lowPlane.Composite(10,10,0),"2x cache does not change 1x composition");
    uint32_t highPixels[4]{};highPlane.Overlay2Rect(highPixels,2,{10,10,11,11});
    CHECK(highPixels[0]==0xFFFFFFFF && highPixels[1]==0 && (highPixels[2]>>24)==128 && highPixels[3]==0xFFFFFFFF,
        "four independent high-grid coverage samples survive composition");
    uint32_t clean=0;highPlane.BackgroundRect(background.data(),w,&clean,1,{10,10,11,11});
    CHECK(clean==0xFF000000,"background upload excludes native compatibility text");
    vt::PixelPlane highCopy(w,h,highOptions);
    highCopy.Copy(&highPlane,{10,10,11,11},{15,10,16,11},background.data(),w,nullptr,0,true,0,0);
    uint32_t copiedPixels[4]{};highCopy.Overlay2Rect(copiedPixels,2,{15,10,16,11});
    CHECK(!memcmp(highPixels,copiedPixels,sizeof(highPixels)),"color-key copy carries all 2x samples");
    highCopy.Copy(&highPlane,{10,10,11,11},{15,10,16,11},background.data(),w,nullptr,0,false,0,0,false,0,0,true,false);
    highCopy.Overlay2Rect(copiedPixels,2,{15,10,16,11});
    CHECK(copiedPixels[0]==highPixels[1] && copiedPixels[1]==highPixels[0] && copiedPixels[2]==highPixels[3],
        "mirrors flip high-grid samples within a logical pixel");
    for(int keyed=0;keyed<2;++keyed) for(int flags=0;flags<4;++flags) {
        vt::PixelPlane before=highCopy,actual=highCopy,expected=highCopy;
        const vt::PixelRect sr{0,0,32,24},dr{8,4,40,28};
        expected.Copy(&before,sr,dr,background.data(),w,background.data(),w,
            keyed!=0,0,0,false,0,0,(flags&1)!=0,(flags&2)!=0);
        actual.Copy(&actual,sr,dr,background.data(),w,background.data(),w,
            keyed!=0,0,0,false,0,0,(flags&1)!=0,(flags&2)!=0);
        uint32_t actualPixels[w*h*4]{},expectedPixels[w*h*4]{};
        actual.Overlay2Rect(actualPixels,w*2,{0,0,w,h});
        expected.Overlay2Rect(expectedPixels,w*2,{0,0,w,h});
        CHECK(!memcmp(actualPixels,expectedPixels,sizeof(actualPixels)),
            "overlapping keyed/mirrored self-copy preserves independent 2x samples");
    }
    // Camera scrolling exercises a full native frame with only sparse text.
    // The old dense snapshot allocated sw*sh PlanePixels for every scroll.
    vt::PixelPlane scroll(1920,1080,highOptions);
    scroll.Paint(highGlyph,400,400,1,0x07E0,{0,0,1920,1080});
    const auto scrollStart=std::chrono::steady_clock::now();
    for(int i=0;i<100;++i) {
        scroll.Copy(&scroll,{0,0,1919,1079},{1,1,1920,1080},nullptr,0);
        scroll.Copy(&scroll,{1,1,1920,1080},{0,0,1919,1079},nullptr,0);
    }
    const double scrollMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-scrollStart).count();
    printf("1920x1080 sparse self-scroll: 200 copies %.3f ms, live tiles=%zu\n",scrollMs,scroll.TileCount());
    CHECK(scroll.At(400,400).high && !scroll.At(401,401).high && scroll.TileCount()==1,
        "full-HD back-and-forth scrolling keeps one green glyph without trails");
    CHECK(scroll.Intersects({400,400,401,401}) && !scroll.Intersects({800,800,801,801}),
        "unrelated world fills do not snapshot text tiles");
    std::fill(background.begin(),background.end(),(unsigned short)0);highPlane.ValidateNative(background.data(),w);
    highPlane.Overlay2Rect(highPixels,2,{10,10,11,11});
    CHECK(highPlane.Empty() && !highPixels[0] && !highPixels[3],"native erase invalidates 1x and 2x together");
    // A high-grid fringe can lie outside the low-grid ink. It is tracked for
    // erasure, but must not add any visible ink to ordinary 1x output.
    highGlyph.cov[0]=0;highPlane.Paint(highGlyph,10,10,1,0xFFFF,{0,0,w,h},background.data(),w);
    CHECK(highPlane.Composite(10,10,0)==0xFF000000 && !highPlane.Empty(),"high-only fringe leaves 1x unchanged");
    highPlane.Clear({10,10,11,11});CHECK(highPlane.Empty(),"rectangular clear includes high-only fringes");
    printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
