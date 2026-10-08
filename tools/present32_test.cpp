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
    // A caption is cached on black once, then copied over changing movie
    // frames. Dark captions need a stable separating white rim even when
    // one line spans both light and dark parts of the scene.
    vt::GlyphRaster2 dotRaster;dotRaster.width=dotRaster.rows=2;dotRaster.coverage={255,255,255,255};
    vt::GlyphCell dot{};dot.inkRows=1;dot.cov[0]=255;dot.bits[0]=0x80;dot.raster2=&dotRaster;
    for(int density : {2,3,4,5,8}) for(bool linear : {false,true}) {
        dotRaster.scale=density;dotRaster.width=dotRaster.rows=density;dotRaster.coverage.assign(density*density,255);
        auto adaptiveOptions=highOptions;adaptiveOptions.linear=linear;
        vt::PixelPlane caption(w,h,adaptiveOptions),movie(w,h,adaptiveOptions);
        std::vector<unsigned short> cached(w*h,0),frame(w*h,0);
        caption.Paint(dot,10,10,1,0x001F,{0,0,w,h},cached.data(),w,true);
        CHECK(caption.Composite(9,10,0)==0xFFFFFFFF,"cached black subtitle surface has a white outer edge");
        for(unsigned short scene : {static_cast<unsigned short>(0),static_cast<unsigned short>(0xFFFF),static_cast<unsigned short>(0)}) {
            movie.Clear({0,0,w,h});std::fill(frame.begin(),frame.end(),scene);
            movie.Copy(&caption,{8,8,13,13},{18,18,23,23},cached.data(),w,frame.data(),w,true,0,0);
            const uint32_t edgeColor=0xFFFFFFFFu;
            CHECK(movie.Composite(19,20,scene)==edgeColor,"dark subtitle separating rim stays white across scene changes");
            CHECK(movie.Composite(20,20,scene)==0xFF0000FFu,"adaptive outline preserves the blue caption body");
            uint32_t borderSamples[4]{};movie.Overlay2Rect(borderSamples,2,{19,20,20,21});
            const int whiteWidth=std::max(1,density/4);
            auto expectedBand=[&](int sx) {
                return sx>=density-whiteWidth ? edgeColor : sx>=density-2*whiteWidth ? 0xFF000000u : 0;
            };
            CHECK(borderSamples[0]==expectedBand(density/4) && borderSamples[1]==expectedBand(3*density/4) &&
                  borderSamples[2]==borderSamples[0] && borderSamples[3]==borderSamples[1],
                "downsampled subtitle retains quarter-pixel white and black bands");
            std::vector<uint32_t> fullBorder(density*density);
            movie.OverlayRect(fullBorder.data(),density,{19,20,20,21},density);
            int whiteSamples=0,blackSamples=0;
            for(int sy=0;sy<density;++sy) for(int sx=0;sx<density;++sx) {
                CHECK(fullBorder[sy*density+sx]==expectedBand(sx),"thin separating rim has exact output-grid coverage");
                whiteSamples+=fullBorder[sy*density+sx]==edgeColor;
                blackSamples+=fullBorder[sy*density+sx]==0xFF000000u;
            }
            CHECK(whiteSamples==density*whiteWidth && blackSamples==whiteSamples,
                "white and black bands have equal support; 5x white band is one sample wide");
            // Mirror must carry edge weights along with independent samples.
            vt::PixelPlane mirrored(w,h,adaptiveOptions);
            mirrored.Copy(&movie,{19,20,20,21},{30,20,31,21},nullptr,0,nullptr,0,false,0,0,false,0,0,true,true);
            std::vector<uint32_t> mirrorSamples(density*density);
            mirrored.OverlayRect(mirrorSamples.data(),density,{30,20,31,21},density);
            bool mirroredBands=true;
            for(int i=0;i<density*density;++i)
                mirroredBands=mirroredBands && fullBorder[i]==mirrorSamples[density*density-1-i];
            CHECK(mirroredBands,"mirrored copies preserve thin white/black edge coverage at its native grid");
            // Emulate the native keyed blit before the post-copy marker pass.
            for(int y=18;y<23;++y) for(int x=18;x<23;++x)
                if(movie.At(x,y).tracked) frame[y*w+x]=movie.At(x,y).marker;
            movie.ResolveSubtitleNative(frame.data(),w,{18,18,23,23});
            CHECK(frame[20*w+19]==(scene ? 0xFFFE : 0xFFFF),"stable native white rim remains observable on matching white backgrounds");
            std::fill(frame.begin(),frame.end(),scene);
            movie.ValidateNative(frame.data(),w);
            CHECK(movie.Empty(),"movie background overwrite erases body and outline together");
            CHECK(movie.RasterScale()==2,"erased high-density frame releases its raster density for later smaller output");
        }
        std::vector<uint32_t> stableRim(5*5*density*density),mixedRim(stableRim.size());
        caption.OverlayRect(stableRim.data(),5*density,{8,8,13,13},density);
        for(int frameIndex=0;frameIndex<8;++frameIndex) {
            movie.Clear({0,0,w,h});
            for(int yy=0;yy<h;++yy) for(int xx=0;xx<w;++xx)
                frame[yy*w+xx]=((xx+yy+frameIndex)&1) ? 0xFFFF : 0;
            movie.Copy(&caption,{8,8,13,13},{18,18,23,23},cached.data(),w,frame.data(),w,true,0,0);
            movie.OverlayRect(mixedRim.data(),5*density,{18,18,23,23},density);
            CHECK(mixedRim==stableRim,"mixed moving backgrounds cannot split or flicker a caption's outline colors");
        }
        vt::PixelPlane clippedEdge(w,h,adaptiveOptions);
        clippedEdge.Paint(dot,10,10,1,0x001F,{10,10,11,11},nullptr,0,true);
        CHECK(!clippedEdge.At(9,10).a && !clippedEdge.At(11,10).high,"outline respects the surface clip");
        for(unsigned short text : {static_cast<unsigned short>(0xFFE0),static_cast<unsigned short>(0xFFFF)}) {
            caption.Clear({0,0,w,h});std::fill(cached.begin(),cached.end(),static_cast<unsigned short>(0));
            caption.Paint(dot,10,10,1,text,{0,0,w,h},cached.data(),w,true);
            for(unsigned short scene : {static_cast<unsigned short>(0),static_cast<unsigned short>(0xFFFF)}) {
                movie.Clear({0,0,w,h});std::fill(frame.begin(),frame.end(),scene);
                movie.Copy(&caption,{8,8,13,13},{18,18,23,23},cached.data(),w,frame.data(),w,true,0,0);
                CHECK(movie.Composite(19,20,scene)==0xFF000000u &&
                    movie.Composite(20,20,scene)==vt::PixelPlane::Expand565(text),
                    "yellow and white subtitle bodies retain a black separating border on dark and bright frames");
                std::vector<uint32_t> samples(density*density);
                movie.OverlayRect(samples.data(),density,{19,20,20,21},density);
                bool thin=true;
                for(int sy=0;sy<density;++sy) for(int sx=0;sx<density;++sx)
                    thin=thin && samples[sy*density+sx]==(sx>=density-density/2 ? 0xFF000000u : 0);
                CHECK(thin,"bright subtitle border occupies only half a logical pixel in the output raster");
                for(int yy=18;yy<23;++yy) for(int xx=18;xx<23;++xx)
                    if(movie.At(xx,yy).tracked) frame[yy*w+xx]=movie.At(xx,yy).marker;
                movie.ResolveSubtitleNative(frame.data(),w,{18,18,23,23});
                CHECK(frame[20*w+19]==1,"fixed black border survives keyed native copies on every scene brightness");
                std::fill(frame.begin(),frame.end(),scene);movie.ValidateNative(frame.data(),w);
                CHECK(movie.Empty(),"fixed black border and bright text erase together without marker residue");
            }
        }
    }
    dotRaster.scale=2;dotRaster.width=dotRaster.rows=2;dotRaster.coverage={255,0,0,0};
    vt::PixelPlane edgedSamples(w,h,highOptions),edgedMirror(w,h,highOptions);
    edgedSamples.Paint(dot,10,10,1,0x001F,{0,0,w,h},nullptr,0,true);
    uint32_t independentEdges[4]{},flippedEdges[4]{};
    edgedSamples.Overlay2Rect(independentEdges,2,{10,10,11,11});
    edgedMirror.Copy(&edgedSamples,{10,10,11,11},{20,20,21,21},nullptr,0,nullptr,0,false,0,0,false,0,0,true,true);
    edgedMirror.Overlay2Rect(flippedEdges,2,{20,20,21,21});
    CHECK(independentEdges[0]==0xFF0000FF && independentEdges[1]==0xFFFFFFFF &&
        flippedEdges[3]==independentEdges[0] && flippedEdges[0]==independentEdges[3],
        "independent high-grid body and adaptive edge samples survive mirrored copies");
    vt::SetPresentationWriter(nullptr);
    for(bool aa : {false,true}) {
        std::fill(background.begin(),background.end(),(unsigned short)0);
        vt::Target target{background.data(),w,0,0,w-1,h-1};
        vt::BeginTextInkCapture(true);
        if(aa) vt::DrawCellAA(target,dot,10,10,1,0x001F,vt::RGB565);
        else vt::DrawCell(target,dot,10,10,1,0x001F);
        vt::TextInkRect dirty{};
        CHECK(vt::EndTextInkCapture(&dirty) && dirty.left==9 && dirty.top==9 && dirty.right==12 && dirty.bottom==12,
            "subtitle erasure includes the full outer border at both raster modes");
        CHECK(background[10*w+9]==0xFFFF && background[10*w+10]==0x001F,"RGB565 fallback paints a border and preserves body color");
        CHECK(!vt::SubtitleOutlineActive(),"subtitle outline scope ends with the completed caption draw");
        std::fill(background.begin(),background.end(),(unsigned short)0xFFFF);
        vt::BeginTextInkCapture(true);
        if(aa) vt::DrawCellAA(target,dot,10,10,1,0x001F,vt::RGB565);
        else vt::DrawCell(target,dot,10,10,1,0x001F);
        vt::EndTextInkCapture(&dirty);
        CHECK(background[10*w+9]==0xFFFF && background[10*w+10]==0x001F,
            "RGB565 dark subtitle rim keeps the same color on bright backgrounds");
        for(unsigned short scene : {static_cast<unsigned short>(0),static_cast<unsigned short>(0xFFFF)}) {
            std::fill(background.begin(),background.end(),scene);
            vt::BeginTextInkCapture(true);
            if(aa) vt::DrawCellAA(target,dot,10,10,1,0xFFE0,vt::RGB565);
            else vt::DrawCell(target,dot,10,10,1,0xFFE0);
            vt::EndTextInkCapture(&dirty);
            CHECK(background[10*w+9]==1 && background[10*w+10]==0xFFE0,
                "native fallback separates yellow subtitle strokes with black on bright and dark scenes");
        }
        std::fill(background.begin(),background.end(),(unsigned short)0);
        vt::DrawCell(target,dot,10,10,1,0x001F);
        CHECK(!background[10*w+9],"ordinary UI text does not receive subtitle borders");
    }
    printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
