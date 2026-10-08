// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GlyphSource.h"
#include "PixelWriter.h"
#include "PixelPlane.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <initializer_list>

// Independent reference: FreeType's bitmap is placed directly on one fixed
// baseline, without fitting/recentring its visible ink inside a 16-row cell.
int main()
{
    vt::GlyphSource source;
    source.SetAntiAlias(true);
    source.SetSupersample(1);
    if (!source.Init("C:\\Windows\\Fonts\\NotoSansSC-VF.ttf", 13, 450, 3, 16, 13)) return 2;
    source.SetSizes(13, 16);
    if (!source.SetLatinFont("C:\\Windows\\Fonts\\arial.ttf")) return 2;
    const wchar_t* sample = L"我们很快就会从三个不同的方向对尤里的巨塔发起攻击。我们必须为悖论引擎开路，将巨塔纳入时间静止的影响范围之内。摧毁厄普西隆总部外围的基地，为我们自己的基地扩张做准备。任务目标一：为总攻行动肃清周边区域。尤里的部队肯定会发起反击，但这些部队足以抵挡他们的反扑。尽快行动，如果你无法尽快清理外围防线的话，他们肯定会派来更多的援军。中文国难普通中高低（）“”‘’——…ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~，。：；！？【】「」%";
    int failures = 0, oldMoves = 0;
    double minShift = 0, maxShift = 0;
    for (const wchar_t* p = sample; *p; ++p) {
        FT_Face face = (FT_Face)source.FaceHandleFor(*p);
        FT_Set_Transform(face, NULL, NULL);
        if (FT_Load_Char(face, *p, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP)) return 2;
        FT_BBox box; FT_Outline_Get_CBox(&face->glyph->outline, &box);
        double top = 13 - box.yMax / 64.0, bottom = 13 - box.yMin / 64.0;
        double shift = top < 0 ? -top : 0;
        if (bottom + shift > 16) shift = 16 - bottom;
        if (fabs(shift) > 0.001) {
            if (oldMoves < 14) printf("old per-character shift U+%04X: %+0.3fpx (raw top=%0.3f bottom=%0.3f)\n", *p, shift, top, bottom);
            ++oldMoves;
            if (shift < minShift) minShift = shift;
            if (shift > maxShift) maxShift = shift;
        }
        for (int ss : {1, 2, 4}) for (int hint : {0, 1, 2})
        for (int phase = 0; phase <= 3; ++phase) for (int scale : {1024, 950}) {
            source.SetSupersample(ss); source.SetHinting(hint);
            face = (FT_Face)source.FaceHandleFor(*p);
            const vt::GlyphCell* cell = source.Get(*p, -1, phase, scale);
            if (!cell) { ++failures; continue; }
            FT_Matrix transform = {scale * 64, 0, 0, scale * 64};
            FT_Vector offset = {phase * 16 * ss, 0};
            FT_Set_Transform(face, &transform, &offset);
            const FT_Int32 flags = (hint == 1 ? FT_LOAD_TARGET_NORMAL : hint == 2 ? FT_LOAD_NO_HINTING : FT_LOAD_TARGET_LIGHT) | FT_LOAD_NO_BITMAP | FT_LOAD_RENDER;
            if (FT_Load_Char(face, *p, flags)) return 2;
            const FT_GlyphSlot g = face->glyph;
            for (int clipCase : {0, 1, 2}) {
                unsigned short actual[64 * 48] = {}, expected[64 * 48] = {};
                unsigned int coverage[64 * 48] = {};
                const int originY = clipCase == 1 ? 0 : 8;
                const int clipT = clipCase == 2 ? 9 : 0, clipB = clipCase == 2 ? 20 : 47;
                vt::Target target = {actual, 64, 0, clipT, 63, clipB};
                vt::DrawCellAA(target, *cell, 8, originY, 16, 0x07FF, vt::RGB565);
                for (unsigned int r = 0; r < g->bitmap.rows; ++r) for (unsigned int c = 0; c < g->bitmap.width; ++c) {
                    int x = (int)floor((8 * ss + g->bitmap_left + (int)c) / (double)ss);
                    int y = (int)floor(((originY + 13) * ss - g->bitmap_top + (int)r) / (double)ss);
                    if (x < 0 || x >= 64 || y < clipT || y > clipB) continue;
                    coverage[y * 64 + x] += g->bitmap.buffer[r * g->bitmap.pitch + c];
                }
                for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) {
                    const int cov = (coverage[y * 64 + x] + ss * ss / 2) / (ss * ss);
                    expected[y * 64 + x] = vt::Blend(0, 0x07FF, cov, vt::RGB565, x, y);
                }
                if (memcmp(actual, expected, sizeof(actual))) ++failures;
            }
        }
        source.SetSupersample(1); source.SetHinting(0);
    }
    printf("old moving glyphs: %d, shifts %+0.3f..%+0.3fpx\n", oldMoves, minShift, maxShift);
    // Explicit widths are also used by direct drawing's fallback. A roomy
    // legacy width must not change that path's vertical shape or baseline.
    for (const wchar_t* p = sample; *p; ++p) {
        const vt::GlyphCell* natural = source.Get(*p, -1);
        const vt::GlyphCell* explicitWidth = source.Get(*p, 24);
        if (!natural || !explicitWidth) { ++failures; continue; }
        if (natural->inkY != explicitWidth->inkY || natural->inkRows != explicitWidth->inkRows)
            ++failures;
        for (int y = 0; y < 48; ++y) {
            int naturalY = y - 8 - natural->inkY, explicitY = y - 8 - explicitWidth->inkY;
            int naturalSum = 0, explicitSum = 0;
            if (naturalY >= 0 && naturalY < natural->inkRows)
                for (int x = 0; x < 24; ++x) naturalSum += natural->cov[naturalY * 24 + x];
            if (explicitY >= 0 && explicitY < explicitWidth->inkRows)
                for (int x = 0; x < 24; ++x) explicitSum += explicitWidth->cov[explicitY * 24 + x];
            // Legacy punctuation alignment shifts the outline fractionally in
            // X, changing gray quantisation by up to one unit per pixel. Its
            // vertical bearing, occupied rows and row coverage must survive.
            if ((naturalSum == 0) != (explicitSum == 0) || abs(naturalSum - explicitSum) > 24) {
                printf("explicit U+%04X y=%d row coverage=%d/%d\n", *p, y, naturalSum, explicitSum);
                ++failures; break;
            }
        }
    }
    // Separate FreeType faces at output resolution verify that the final 2x
    // layer includes every raster row, including ink above the logical top.
    vt::GlyphSource reference2;
    reference2.SetAntiAlias(true);
    if(!reference2.Init("C:\\Windows\\Fonts\\NotoSansSC-VF.ttf",26,450,3,32,26)) return 2;
    reference2.SetSizes(26,32);
    if(!reference2.SetLatinFont("C:\\Windows\\Fonts\\arial.ttf")) return 2;
    source.SetHighResolution(true);
    int highChecks=0;
    for(const wchar_t* p=sample;*p;++p) for(int hint:{0,1,2})
    for(int phase:{0,1,2,3}) for(int scale:{1024,950}) {
        source.SetHinting(hint);
        const auto* cell=source.Get(*p,-1,phase,scale);
        if(!cell || !cell->raster2) { ++failures;continue; }
        FT_Face face=(FT_Face)reference2.FaceHandleFor(*p);
        FT_Matrix transform{scale*64,0,0,scale*64};FT_Vector offset{phase*32,0};
        FT_Set_Transform(face,&transform,&offset);
        FT_Int32 flags=(hint==1 ? FT_LOAD_TARGET_NORMAL : hint==2 ? FT_LOAD_NO_HINTING : FT_LOAD_TARGET_LIGHT)|FT_LOAD_NO_BITMAP|FT_LOAD_RENDER;
        if(FT_Load_Char(face,*p,flags)) return 2;
        const auto g=face->glyph;
        vt::PlaneOptions options;options.highResolution=true;
        vt::PixelPlane plane(64,48,options);
        plane.Paint(*cell,8,8,16,0xFFFF,{0,0,64,48});
        unsigned int actual[128*96]{},expected[128*96]{};
        plane.Overlay2Rect(actual,128,{0,0,64,48});
        for(unsigned int r=0;r<g->bitmap.rows;++r) for(unsigned int c=0;c<g->bitmap.width;++c) {
            int x=16+g->bitmap_left+c,y=42-g->bitmap_top+r;
            if(x>=0 && x<128 && y>=0 && y<96) expected[y*128+x]=g->bitmap.buffer[r*g->bitmap.pitch+c];
        }
        bool mismatch=false;
        for(int i=0;i<128*96;++i) if((actual[i]>>24)!=expected[i]) { mismatch=true;break; }
        if(mismatch) { if(failures<10) printf("2x mismatch U+%04X hint=%d phase=%d scale=%d\n",*p,hint,phase,scale);++failures; }
        ++highChecks;
    }
    printf("2x independent font/placement comparisons: %d\n",highChecks);
    int outputChecks=0;
    for(int density : {3,4,5,8}) for(const wchar_t* p=L"读取中版权©汉Ag，jg";*p;++p)
    for(int hint : {0,2}) for(int phase : {0,3}) {
        source.SetHighResolutionScale(density);source.SetHinting(hint);
        const auto* cell=source.Get(*p,-1,phase);
        if(!cell || !cell->raster2 || cell->raster2->scale!=density) { ++failures;continue; }
        FT_Face face=(FT_Face)reference2.FaceHandleFor(*p);
        const int pixels=(*p>=0x2E80 ? 16 : 13)*density;
        if(FT_Set_Pixel_Sizes(face,0,pixels)) return 2;
        FT_Matrix transform{65536,0,0,65536};FT_Vector offset{phase*16*density,0};
        FT_Set_Transform(face,&transform,&offset);
        if(FT_Load_Char(face,*p,(hint==2 ? FT_LOAD_NO_HINTING : FT_LOAD_TARGET_LIGHT)|FT_LOAD_NO_BITMAP|FT_LOAD_RENDER)) return 2;
        const auto g=face->glyph;
        vt::PlaneOptions options;options.highResolution=true;vt::PixelPlane plane(32,32,options);
        plane.Paint(*cell,4,4,16,0xFFFF,{0,0,32,32});
        std::vector<unsigned int> actual(32*32*density*density),expected(actual.size());
        plane.OverlayRect(actual.data(),32*density,{0,0,32,32},density);
        for(unsigned int r=0;r<g->bitmap.rows;++r) for(unsigned int c=0;c<g->bitmap.width;++c) {
            const int x=4*density+g->bitmap_left+c,y=17*density-g->bitmap_top+r;
            if(x>=0 && x<32*density && y>=0 && y<32*density)
                expected[y*32*density+x]=g->bitmap.buffer[r*g->bitmap.pitch+c];
        }
        bool match=true;for(size_t i=0;i<actual.size();++i) if((actual[i]>>24)!=expected[i]) {match=false;break;}
        if(!match) {printf("native grid mismatch U+%04X raster=%d hint=%d phase=%d\n",*p,density,hint,phase);++failures;}
        ++outputChecks;
    }
    printf("3x/4x/5x/8x independent output-resolution comparisons: %d\n",outputChecks);
    // Quarter-pixel Y positioning must rasterise from outlines at each output
    // density. Compare independent FreeType coverage for letters, descenders,
    // punctuation and raised symbols; none gets a special symbol adjustment.
    int verticalChecks=0;
    for(int latinSize:{13,16}) for(int ss:{1,2}) for(int density:{1,2,5})
    for(unsigned int cp:{(unsigned int)'H',(unsigned int)'g',(unsigned int)'%',0x2122u})
    for(int verticalPhase:{-2,-1,0,1,2}) {
        source.SetSizes(latinSize,16);source.SetSupersample(ss);source.SetHinting(2);
        source.SetHighResolution(density>1);source.SetHighResolutionScale(density);
        const auto* cell=source.Get(cp,-1,0,1024,verticalPhase);
        if(!cell) {++failures;continue;}
        auto face=(FT_Face)reference2.FaceHandleFor(cp);
        const int raster=density>1 ? density : ss;
        if(FT_Set_Pixel_Sizes(face,0,latinSize*raster)) return 2;
        FT_Vector offset{0,-verticalPhase*16*raster};
        FT_Set_Transform(face,NULL,&offset);
        if(FT_Load_Char(face,cp,FT_LOAD_NO_HINTING|FT_LOAD_NO_BITMAP|FT_LOAD_RENDER)) return 2;
        const auto glyph=face->glyph;
        const int width=32*density,height=32*density;
        std::vector<unsigned int> expected(width*height),actual(expected.size());
        for(unsigned int r=0;r<glyph->bitmap.rows;++r) for(unsigned int c=0;c<glyph->bitmap.width;++c) {
            const int x=(int)floor((4*raster+glyph->bitmap_left+(int)c)*(double)density/raster);
            const int y=(int)floor((17*raster-glyph->bitmap_top+(int)r)*(double)density/raster);
            if(x>=0 && x<width && y>=0 && y<height) expected[y*width+x]+=glyph->bitmap.buffer[r*glyph->bitmap.pitch+c];
        }
        bool match=true;
        if(density>1) {
            vt::PlaneOptions options;options.highResolution=true;vt::PixelPlane plane(32,32,options);
            plane.Paint(*cell,4,4,16,0xFFFF,{0,0,32,32});
            plane.OverlayRect(actual.data(),width,{0,0,32,32},density);
            for(size_t i=0;i<actual.size();++i) if((actual[i]>>24)!=expected[i]) {match=false;break;}
        } else {
            unsigned short native[32*32]{};vt::Target target{native,32,0,0,31,31};
            vt::DrawCellAA(target,*cell,4,4,16,0x07FF,vt::RGB565);
            for(int y=0;y<32;++y) for(int x=0;x<32;++x) {
                const int coverage=(expected[y*32+x]+ss*ss/2)/(ss*ss);
                if(native[y*32+x]!=vt::Blend(0,0x07FF,coverage,vt::RGB565,x,y)) match=false;
            }
        }
        if(!match) {printf("vertical phase mismatch U+%04X size=%d ss=%d density=%d phase=%d\n",cp,latinSize,ss,density,verticalPhase);++failures;}
        ++verticalChecks;
    }
    printf("independent vertical-phase comparisons: %d\n",verticalChecks);
    printf("%s: %d fixed-baseline bitmap comparisons failed\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
