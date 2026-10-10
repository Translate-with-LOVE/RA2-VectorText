// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "../src/GlyphSource.h"
#include "../src/PixelWriter.h"
#include "../src/Takeover.h"
#include "../src/Logger.h"
#include "../src/PixelPlane.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include <string>

// Uses the production takeover and a real game.fnt table.
// Run beside an INI copy, so QA never changes the game's config or log.
#pragma pack(push, 1)
struct FontData {
    unsigned int unknown, stride, lines, unknownC, unknown10, bytes;
    const unsigned short* symbols;
    unsigned char* bitmaps;
};
#pragma pack(pop)

static vt::PixelPlane* previewPlane = nullptr;
static int previewRaster = 2;
static int PaintPreview(const vt::Target& target, const vt::GlyphCell& cell,
                        int x, int y, int rows, unsigned short color, bool) {
    previewPlane->Paint(cell, x, y, rows, color,
        {target.clipL, target.clipT, target.clipR + 1, target.clipB + 1}, target.base, target.pitch,
        vt::SubtitleOutlineActive());
    return 1;
}

static void WriteBmp(const char* path, const std::vector<unsigned short>& pixels, int w, int h,
                     const vt::PixelPlane* plane = nullptr, bool guides = false, int rowCount = 0, int rowStep = 23,
                     bool hidpi = false, bool linear = true) {
    const int scale = hidpi ? previewRaster : 1, outputW = w * scale, outputH = h * scale;
    // Use the same independent 2x samples and encoded overlay as D3D9.
    // The base scene is nearest-neighbour here; cnc-ddraw's scene shader is
    // deliberately outside this offline text preview.
    std::vector<unsigned int> overlay;
    if (hidpi) {
        overlay.resize((size_t)outputW * outputH);
        plane->OverlayRect(overlay.data(), outputW, {0, 0, w, h},scale);
    }
    int pitch = (outputW * 3 + 3) & ~3;
    std::vector<unsigned char> bmp(54 + pitch * outputH, 0);
    bmp[0] = 'B'; bmp[1] = 'M';
    *(unsigned int*)&bmp[2] = (unsigned int)bmp.size();
    *(unsigned int*)&bmp[10] = 54; *(unsigned int*)&bmp[14] = 40;
    *(int*)&bmp[18] = outputW; *(int*)&bmp[22] = outputH;
    *(unsigned short*)&bmp[26] = 1; *(unsigned short*)&bmp[28] = 24;
    for (int oy = 0; oy < outputH; ++oy) for (int ox = 0; ox < outputW; ++ox) {
        const int x = ox / scale, y = oy / scale;
        unsigned int c = plane ? plane->Composite(x, y, pixels[y * w + x]) : vt::PixelPlane::Expand565(pixels[y * w + x]);
        if (hidpi) {
            const auto p = plane->At(x, y);
            const unsigned int background = vt::PixelPlane::Expand565(p.tracked ? p.background : pixels[y * w + x]);
            const unsigned int ink = overlay[(size_t)oy * outputW + ox];
            const double alpha = (ink >> 24) / 255.0;
            c = 0;
            for (int shift : {0, 8, 16}) {
                double fg = ((ink >> shift) & 255) / 255.0, bg = ((background >> shift) & 255) / 255.0;
                if (linear) { fg = pow(fg, 2.2); bg = pow(bg, 2.2); }
                double value = fmin(1.0, fg + bg * (1.0 - alpha));
                if (linear) value = pow(value, 1.0 / 2.2);
                c |= (unsigned int)floor(value * 255.0 + 0.5) << shift;
            }
        }
        if (guides && x >= 4 && ((x-4)%8)<4) for (int row=0;row<rowCount;++row) {
            const int relative=y-(6+row*rowStep);
            const unsigned int guide=relative==0?0xEF6666:relative==8?0x73AAFF:relative==16?0xFFCC55:0;
            if (!guide) continue;
            // Preview overlay only. Keep the glyph's real raster untouched.
            unsigned int result=0;
            for (int shift=0;shift<=16;shift+=8)
                result|=((((c>>shift)&255)*3+((guide>>shift)&255)*2)/5)<<shift;
            c=result;
        }
        unsigned char* p = &bmp[54 + (outputH - oy - 1) * pitch + ox * 3];
        p[0] = (unsigned char)c;
        p[1] = (unsigned char)(c >> 8);
        p[2] = (unsigned char)(c >> 16);
    }
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(&bmp[0], 1, bmp.size(), f); fclose(f); }
}

int main(int argc, char** argv) {
    const char* output = argc > 1 ? argv[1] : "preview.bmp";
    bool check = false, line = false, scene = false, bgra = false, guides = false, hidpi = false, loading = false;
    bool subtitles = false;
    bool yellowSubtitles = false;
    bool startup = false;
    const char* movieBackground = nullptr;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--check")) check = true;
        if (!strcmp(argv[i], "--line")) line = true;
        if (!strcmp(argv[i], "--scene")) scene = true;
        if (!strcmp(argv[i], "--bgra")) bgra = true;
        if (!strcmp(argv[i], "--guides")) guides = true;
        if (!strcmp(argv[i], "--hidpi")) hidpi = true;
        if (!strcmp(argv[i], "--loading")) loading = true;
        if (!strcmp(argv[i], "--subtitles")) subtitles = true;
        if (!strcmp(argv[i], "--yellow-subtitles")) subtitles = yellowSubtitles = true;
        if (!strcmp(argv[i], "--startup")) startup = true;
        if (!strcmp(argv[i], "--movie-background") && i+1<argc) movieBackground=argv[++i];
        if (!strcmp(argv[i], "--raster") && i+1<argc) previewRaster=std::max(2,std::min(8,atoi(argv[++i])));
    }
    if ((hidpi && !bgra) || (guides && (hidpi || loading))) {
        printf("--hidpi requires --bgra; --guides uses the standard 1x samples\n"); return 2;
    }
    FILE* f = fopen(VT_GAME_DIR "/game.fnt", "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
    std::vector<unsigned char> data(len);
    fread(&data[0], 1, len, f); fclose(f);
    std::vector<unsigned char> original = data;
    FontData fd = {};
    const unsigned int* hdr = (const unsigned int*)&data[0];
    fd.stride = hdr[2]; fd.lines = hdr[3]; fd.bytes = hdr[6];
    fd.symbols = (const unsigned short*)&data[28];
    fd.bitmaps = &data[28 + 131072];
    std::vector<const wchar_t*> rows = {
        L"Phobos development build #48. Please test the build before shipping.",
        L"难度：普通",
        L"我们快到了。我们的部队已经抵达了尤里邪恶阴谋的核心。",
        L"我们很快就会从三个不同的方向对尤里的巨塔发起攻击。",
        L"我们必须守住它，将巨塔的人造晶体上的影响范围扩大。",
        L"Aa Bb Dd Gg Hh Ii Jj Ll Mm Ww  0123456789  .,;:!?()  。 ，：！？",
        L"任务目标一：为总攻行动肃清周边区域。",
        L"指挥官，命运和我们开了一个天大的玩笑，我们终于解开了尤里奇袭伦敦之谜——这让西格弗里德都大为震惊。",
        L"摧毁厄普西隆总部外围的基地， 为我们自己的基地扩张做准备。",
        L"他说：“保护‘心灵终结仪’，然后撤离。”",
        L"（中文）【任务目标】「保护基地」——完成……",
        L"中文，中文。中文：中文；中文！中文？中文、中文。",
        L"Mixed (中文) [MIDAS] 04:12, done. Test: 50%; OK!"
    };
    if (loading) rows = {
        L"军事行动: 风暴使者 - 地点: 维尔京群岛",
        L"任务目标一: 保护天气控制机",
        L"任务目标二: 消灭敌军部队"
    };
    if (subtitles) rows.assign(6,L"我相信可以用它将部队送回之前的时间，");
    if (yellowSubtitles) rows.assign(6,L"尤里的部队已经成功地启动了两部心灵控制器装置，");
    if (movieBackground && subtitles) {
        rows.resize(2);
        if(!yellowSubtitles) rows.assign(2,L"如果我的装置顺利运作了，谭雅小姐，");
    }
    if (startup) rows={L"读取中 ...",L"© 2000, 2001 美国艺电公司 保留所有权利",
        L"WESTWOOD STUDIOS 是美国艺电的一个品牌",L"命令与征服 以及 尤里的复仇 是商标或注册商标",
        L"美国艺电股份有限公司在美国和/或其他国家的商标。"};
    if(hidpi) vt::SetOutputRasterScale(previewRaster);
    const int rowCount = (int)rows.size();
    unsigned char bf[128] = {};
    *(void**)(bf + 4) = &fd;
    int w = movieBackground ? 800 : subtitles ? 320 : startup ? 440 : loading ? 420 : 720;
    const int rowStep = guides ? 40 : 23;
    const int h = movieBackground ? 600 : (subtitles || startup) ? rowCount*rowStep+12 : loading ? 104 : guides ? rowCount*rowStep+12 : 320;
    const auto rowY = [&](int row) { return movieBackground ? h-76+row*rowStep : loading && row ? 56 + (row-1)*20 : 6 + row*rowStep; };
    // The preview canvas must contain the complete natural-width sample.
    // Measurement does not require a locked surface; allow both side margins.
    if (line) for (const wchar_t* row : rows) {
        int measured = 0;
        if (!vt::Takeover::MeasureDynamicWidth(bf, row, 0, &measured)) return 3;
        if (measured + 8 > w) w = measured + 8;
    }
    std::vector<unsigned short> pixels(w * h, 0);
    if (scene) {
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            // Deterministic dark terrain-like RGB565 background. The bottom
            // sample also exercises white mixed text on a flat UI grey.
            unsigned int noise = ((unsigned int)x * 1664525u + (unsigned int)y * 1013904223u) >> 25;
            int red = 3 + (noise & 3), green = 10 + (noise & 7), blue = 3 + (noise & 1);
            if (y >= 270) { red = blue = 3; green = 6; }
            pixels[y * w + x] = (unsigned short)((red << 11) | (green << 5) | blue);
        }
    }
    if(subtitles && !movieBackground) for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        const int pair=std::min(2,y/(rowStep*2));
        const int grey=pair==0 ? 0 : pair==1 ? 255 : ((x/12+y/9)%2 ? 210 : 25);
        pixels[y*w+x]=(unsigned short)(((grey*31/255)<<11)|((grey*63/255)<<5)|(grey*31/255));
    }
    if(movieBackground) {
        HBITMAP bitmap=(HBITMAP)LoadImageA(nullptr,movieBackground,IMAGE_BITMAP,0,0,LR_LOADFROMFILE|LR_CREATEDIBSECTION);
        BITMAP info{};
        if(!bitmap || !GetObject(bitmap,sizeof(info),&info) || info.bmWidth!=w || info.bmHeight!=h) {
            if(bitmap) DeleteObject(bitmap);
            printf("FAIL: movie background must be an 800x600 BMP\n");return 2;
        }
        BITMAPINFO format{};format.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        format.bmiHeader.biWidth=w;format.bmiHeader.biHeight=-h;
        format.bmiHeader.biPlanes=1;format.bmiHeader.biBitCount=32;format.bmiHeader.biCompression=BI_RGB;
        std::vector<uint32_t> rgb(w*h);
        HDC dc=CreateCompatibleDC(nullptr);
        const int copied=GetDIBits(dc,bitmap,0,h,rgb.data(),&format,DIB_RGB_COLORS);
        DeleteDC(dc);DeleteObject(bitmap);
        if(copied!=h) return 2;
        for(size_t i=0;i<rgb.size();++i) pixels[i]=(unsigned short)((((rgb[i]>>19)&31)<<11)|(((rgb[i]>>10)&63)<<5)|((rgb[i]>>3)&31));
    }
    vt::PlaneOptions planeOptions;
    planeOptions.linear = vt::Cfg::LinearBlend(); planeOptions.gamma = vt::Cfg::Gamma();
    planeOptions.antialias = vt::Cfg::AntiAlias(); planeOptions.outline = vt::Cfg::Outline();
    planeOptions.outlineColor = vt::Cfg::OutlineColor();
    planeOptions.highResolution = hidpi;
    vt::PixelPlane plane(w, h, planeOptions);
    if (bgra) { previewPlane = &plane; vt::SetPresentationWriter(PaintPreview); }
    *(void**)(bf + 4) = &fd; *(void**)(bf + 12) = &pixels[0];
    *(int*)(bf + 16) = w;
    *(int*)(bf + 56) = w - 1; *(int*)(bf + 60) = h - 1;
    int fails = 0;
    for (int r = 0; r < rowCount; ++r) {
        *(unsigned short*)(bf + 36) = subtitles ? (yellowSubtitles ? 0xFFE0 : 0x001F) : (loading || startup) ? 0xFFFF : r == 0 ? 0xF800 : (r >= 5 ? 0xFFFF : 0x07FF);
        if(subtitles) vt::BeginTextInkCapture((r&1)!=0);
        int x = 4, y = rowY(r);
        if(movieBackground) {
            int measured=0;
            if(!vt::Takeover::MeasureDynamicWidth(bf,rows[r],0,&measured)) return 3;
            x=std::max(4,(w-measured)/2);
        }
        if (line && !vt::Takeover::BeginLine(bf, rows[r], -1, x, y, x, 0, 0, 0x43464D)) ++fails;
        for (const wchar_t* p = rows[r]; *p; ++p) {
            int oldWidth = fd.bitmaps[(fd.symbols[*p] - 1) * fd.bytes];
            int nextX = -1;
            bool drawn = line && vt::Takeover::TryLineBlit(bf, *p, x, y, -1, 0x43464D, &nextX);
            if (!drawn) drawn = vt::Takeover::TryBlit(bf, *p, x, y, -1, &nextX);
            if (!drawn) ++fails;
            if (nextX != x + oldWidth) ++fails;
            x = nextX;
        }
        vt::Takeover::EndLine(bf);
        if(subtitles) { vt::TextInkRect ink{};vt::EndTextInkCapture(&ink); }
    }
    if (data != original) { printf("FAIL: game metrics/data changed\n"); ++fails; }
    if (bgra) {
        bool touchesEdge = false;
        bool independentSamples = false;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const auto p = plane.At(x, y);
            if (p.high) for (int i = 1; i < 4; ++i)
                independentSamples |= p.samples[i].a != p.samples[0].a;
            if(p.grid) for(const auto& sample:p.grid->pixels) independentSamples |= sample.a!=p.grid->pixels[0].a;
        }
        if (hidpi && !independentSamples) { printf("FAIL: 2x preview has no independent output samples\n"); ++fails; }
        for (int x = 0; x < w; ++x)
            touchesEdge |= plane.At(x, 0).a != 0 || plane.At(x, h - 1).a != 0;
        for (int y = 0; y < h; ++y)
            touchesEdge |= plane.At(0, y).a != 0 || plane.At(w - 1, y).a != 0;
        if (touchesEdge) { printf("FAIL: preview ink touches canvas edge\n"); ++fails; }
    }
    WriteBmp(output, pixels, w, h, bgra ? &plane : nullptr, guides, rowCount, rowStep, hidpi, planeOptions.linear);
    if (guides) {
        FILE* metadata=fopen((std::string(output)+".json").c_str(),"wb");
        if(!metadata) return 4;
        fprintf(metadata,"{\"cellHeight\":16,\"baseline\":%d,\"rows\":[",vt::Cfg::BaselineRow());
        for(int r=0;r<rowCount;++r) {
            vt::Takeover::InkY ink={};
            int measured=0;
            if(!vt::Takeover::MeasureTextInkY(bf,rows[r],4,0,&ink) ||
               !vt::Takeover::MeasureDynamicWidth(bf,rows[r],0,&measured)) {fclose(metadata);return 5;}
            fprintf(metadata,"%s{\"index\":%d,\"y\":%d,\"inkTop\":%d,\"inkBottom\":%d,\"width\":%d}",
                    r?",":"",r,6+r*rowStep,ink.top,ink.bottom,measured);
        }
        fprintf(metadata,"]}\n");fclose(metadata);
    }
    vt::SetPresentationWriter(nullptr); previewPlane = nullptr;
    // Repeat draws at different origins must preserve original font metrics.
    for (int n = 0; n < 100; ++n) {
        int x = 3 + (n % 7), y = 29;
        for (const wchar_t* p = rows[1]; *p; ++p) {
            int expected = fd.bitmaps[(fd.symbols[*p] - 1) * fd.bytes], nx;
            vt::Takeover::TryBlit(bf, *p, x, y, -1, &nx);
            if (nx != x + expected) ++fails;
            x = nx;
        }
    }
    // Direct glyph checks supplement the surface/advance checks.
    vt::GlyphSource glyphs;
    glyphs.SetAntiAlias(true); glyphs.SetSupersample(vt::Cfg::Supersample());
    glyphs.SetHinting(vt::Cfg::ConfigInt("Hinting", 0));
    glyphs.Init(vt::Cfg::FontFile(), vt::Cfg::FontSizeLatin(), vt::Cfg::FontWeight(), 3, 16, vt::Cfg::BaselineRow());
    glyphs.SetSizes(vt::Cfg::FontSizeLatin(), vt::Cfg::FontSize());
    char latinFont[MAX_PATH] = {};
    vt::Cfg::ConfigStr("FontFileLatin", "", latinFont, sizeof(latinFont));
    if (!glyphs.SetLatinFont(latinFont, vt::Cfg::FontWeightLatin()))
        glyphs.SetLatinFont("", vt::Cfg::FontWeightLatin());
    if (glyphs.FaceHandleFor(0x2014) != glyphs.FaceHandleFor(0x4E2D) ||
        glyphs.FaceHandleFor(0x201C) != glyphs.FaceHandleFor(0x4E2D) ||
        glyphs.FaceHandleFor('.') != glyphs.FaceHandle()) {
        printf("FAIL: mixed punctuation uses wrong face\n"); ++fails;
    }
    const vt::GlyphCell* narrow = glyphs.Get('M', 3, 0);
    const vt::GlyphCell* wide = glyphs.Get('M', 7, 0);
    if (narrow == wide || narrow->width != 3 || wide->width != 7) ++fails;
    const vt::GlyphCell* p0 = glyphs.Get('W', 11, 0);
    const vt::GlyphCell* p2 = glyphs.Get('W', 11, 2);
    if (p0 == p2) { printf("FAIL: subpixel phase cache collision\n"); ++fails; }
    // A rasterisation choice must not change natural line spacing. Compare
    // design advances across target-size normal/light and 4x light rendering.
    vt::GlyphSource metrics;
    metrics.SetAntiAlias(true); metrics.SetSupersample(1);
    metrics.Init(vt::Cfg::FontFile(), vt::Cfg::FontSizeLatin(), vt::Cfg::FontWeight(), 3, 16, vt::Cfg::BaselineRow());
    metrics.SetSizes(vt::Cfg::FontSizeLatin(), vt::Cfg::FontSize());
    if (!metrics.SetLatinFont(latinFont, vt::Cfg::FontWeightLatin()))
        metrics.SetLatinFont("", vt::Cfg::FontWeightLatin());
    const wchar_t* metricSample = L"AVWim0g\x4E2D\x56FD\x3002";
    for (const wchar_t* p = metricSample; *p; ++p) {
        metrics.SetSupersample(1); metrics.SetHinting(0);
        const vt::GlyphCell* light = metrics.Get(*p, -1);
        if (!light) { ++fails; continue; }
        int expectedQ = light->advanceQ;
        metrics.SetHinting(1);
        const vt::GlyphCell* normal = metrics.Get(*p, -1);
        if (!normal || normal->advanceQ != expectedQ) { printf("FAIL: hinting changes U+%04X advance\n", *p); ++fails; }
        metrics.SetHinting(0); metrics.SetSupersample(4);
        const vt::GlyphCell* sampled = metrics.Get(*p, -1);
        if (!sampled || sampled->advanceQ != expectedQ) { printf("FAIL: sampling changes U+%04X advance\n", *p); ++fails; }
        metrics.SetSupersample(1);
        const vt::GlyphCell* phased = metrics.Get(*p, -1, 2);
        if (!phased || phased->advanceQ != expectedQ) { printf("FAIL: phase changes U+%04X advance\n", *p); ++fails; }
        const vt::GlyphCell* scaled = metrics.Get(*p, -1, 0, 950);
        if (!scaled || abs(scaled->advanceQ - (int)floor(expectedQ * 950.0 / 1024 + 0.5)) > 1) {
            printf("FAIL: scaled U+%04X natural advance is inconsistent\n", *p); ++fails;
        }
    }
    const vt::GlyphCell* box = glyphs.Get(0x56FD, 14, 0); // 国: continuous side strokes
    int top = -1, bottom = -1, blank = 0;
    for (int y = 0; y < 16; ++y) {
        int sum = 0; for (int x = 0; x < 24; ++x) sum += box->cov[y * 24 + x];
        if (sum) { if (top < 0) top = y; bottom = y; }
    }
    for (int y = top; y <= bottom; ++y) {
        int sum = 0; for (int x = 0; x < 24; ++x) sum += box->cov[y * 24 + x];
        if (!sum) ++blank;
    }
    printf("CJK box: occupied rows %d..%d, internal blank rows %d\n", top, bottom, blank);
    if (blank) ++fails;
    const wchar_t* sample = L"国难普部。，：；！？、（）【】「」“”‘’——…AaWMi.,:;!?()";
    for (const wchar_t* p = sample; *p; ++p) {
        int adv = fd.bitmaps[(fd.symbols[*p] - 1) * fd.bytes];
        const vt::GlyphCell* cell = glyphs.Get(*p, adv, 0);
        for (int y = 0; y < 16; ++y) for (int x = adv; x < 24; ++x)
            if (cell->cov[y * 24 + x]) {
                printf("FAIL: U+%04X coverage %d at x=%d exceeds advance %d\n", *p, cell->cov[y * 24 + x], x, adv);
                ++fails; y = 16; break;
            }
    }
    vt::SetDither(true);
    vt::SetLinearBlend(true);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        int last = 0;
        for (int cov = 0; cov <= 255; ++cov) {
            unsigned short c = vt::Blend(0, 0xF800, cov, vt::RGB565, x, y);
            int red = c >> 11;
            if ((c & 0x07FF) || red < last) ++fails;
            last = red;
            if (vt::Blend(0x1234, 0x1234, cov, vt::RGB565, x, y) != 0x1234) ++fails;
        }
    }
    printf("%s: %d failures; image %s\n", fails ? "FAIL" : "PASS", fails, output);
    return check && fails ? 1 : 0;
}
