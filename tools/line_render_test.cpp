// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Takeover.h"
#include "GlyphSource.h"
#include "PixelWriter.h"
#include "Logger.h"
#include "SyringeABI.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <process.h>
#include <vector>

static int failures = 0;
#define CHECK(c, label) do { if (!(c)) { ++failures; printf("FAIL: %s\n", label); } } while (0)
static const int W = 720, H = 64;
static const unsigned int Caller = 0x43464D;
static std::vector<unsigned char> fontData;
static unsigned short pixels[W * H];
static unsigned char font[128];
static unsigned short* map;
static unsigned char* bitmaps;
#pragma pack(push, 1)
struct Internal { unsigned int unknown, stride, lines, uC, u10, bytes; void* map; void* data; };
#pragma pack(pop)
static Internal fd;

static int Advance(unsigned int cp) { return bitmaps[(map[cp] - 1) * fd.bytes] + 1; }
static int LegacyWidth(const wchar_t* text)
{
    int w = 0; for (; *text; ++text) w += Advance(*text); return w;
}
static void Clear() { memset(pixels, 0, sizeof(pixels)); vt::Takeover::EndLine(NULL); }
static int Lit() { int n = 0; for (unsigned short p : pixels) if (p) ++n; return n; }
static void Draw(const wchar_t* s, int x, int y, int limit = 2048, int color = -1)
{
    for (int i = 0; s[i] && i < limit; ++i)
    {
        int end = -1;
        CHECK(vt::Takeover::TryLineBlit(font, s[i], x, y, color, Caller, &end), "prepared glyph accepted");
        CHECK(end == x + Advance(s[i]), "engine receives width plus tracking, not natural X");
        x = end;
    }
}
static unsigned int __stdcall ThreadProbe(void*)
{
    vt::Takeover::LineInfo info;
    if (vt::Takeover::GetLineInfo(&info)) return 1;
    if (!vt::Takeover::BeginLine(font, L"AV", -1, 30, 12, 30, 0, 0, Caller)) return 2;
    vt::Takeover::EndLine(font);
    return 0;
}

#include "dimension_machine_check.h"
#include "config_encoding_check.h"

int main(int argc, char** argv)
{
    if (argc>2 && !strcmp(argv[1],"--config-encoding")) return CheckConfigEncoding(argv[2]);
    CHECK(GetModuleHandleA("Phobos.dll") == NULL && GetModuleHandleA("Ares.dll") == NULL,
          "regression process starts without Phobos or Ares");
    FILE* f = fopen(VT_GAME_DIR "/game.fnt", "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
    fontData.resize(len); fread(fontData.data(), 1, len, f); fclose(f);
    const std::vector<unsigned char> original = fontData;
    const unsigned int* hdr = (const unsigned int*)fontData.data();
    map = (unsigned short*)(fontData.data() + 28); bitmaps = fontData.data() + 131100;
    fd = { 0, hdr[2], hdr[3], 0, 0, hdr[6], map, bitmaps };
    *(void**)(font + 4) = &fd; *(void**)(font + 12) = pixels; *(int*)(font + 16) = W;
    *(unsigned short*)(font + 36) = 0x07FF; *(int*)(font + 40) = 32; *(int*)(font + 44) = 1;
    *(int*)(font + 56) = W - 1; *(int*)(font + 60) = H - 1;
    CHECK(vt::Takeover::LineEnabled(), "line mode enabled in installed test INI");
    vt::Takeover::LineInfo info, before;
    const wchar_t* text = L"AVATAR Mix (中文), 50%.";
    Clear();
    CHECK(vt::Takeover::BeginLine(font, text, -1, 20, 12, 20, 0, 0, Caller), "mixed line prepared");
    CHECK(!Lit(), "planning writes no pixels");
    CHECK(vt::Takeover::GetLineInfo(&info), "line metrics exposed");
    int dynamicWidth = -1;
    before = info;
    void* lockedPixels = *(void**)(font + 12);
    *(void**)(font + 12) = NULL;
    CHECK(vt::Takeover::MeasureDynamicWidth(font, text, 0, &dynamicWidth), "natural width measured without locking a surface");
    CHECK(dynamicWidth == (info.widthQ + 3) / 4 + 4, "measure uses drawing's kerning and mixed punctuation plus four pixels");
    *(void**)(font + 12) = lockedPixels;
    vt::Takeover::GetLineInfo(&info);
    CHECK(!memcmp(&info, &before, sizeof(info)) && !Lit(), "measurement preserves active line and writes no pixels");
    Draw(text, 20, 12);
    CHECK(Lit() > 0, "line draws on locked surface");
    const wchar_t* legacyQuotes=L"中文\x0093中文\x0094 A\x0092V\x0085";
    const wchar_t* unicodeQuotes=L"中文\x201C中文\x201D A\x2019V\x2026";
    if (vt::Cfg::LegacyCodepage1252()) {
        Clear();
        CHECK(vt::Takeover::BeginLine(font,legacyQuotes,-1,20,12,20,0,0,Caller),"legacy punctuation row prepared using original engine characters");
        int legacyMeasure=0,unicodeMeasure=0;
        vt::Takeover::MeasureDynamicWidth(font,legacyQuotes,0,&legacyMeasure);
        Draw(legacyQuotes,20,12);
        const std::vector<unsigned short> legacyPixels(pixels,pixels+W*H);
        Clear();
        CHECK(vt::Takeover::BeginLine(font,unicodeQuotes,-1,20,12,20,0,0,Caller),"correct Unicode reference row prepared");
        vt::Takeover::MeasureDynamicWidth(font,unicodeQuotes,0,&unicodeMeasure);
        Draw(unicodeQuotes,20,12);
        CHECK(legacyMeasure==unicodeMeasure && !memcmp(pixels,legacyPixels.data(),sizeof(pixels)),
              "legacy punctuation matches Unicode layout and pixels while returning each original engine advance");
        Clear();
        CHECK(vt::Takeover::BeginLine(font,L"WESTWOOD\x0099",-1,20,12,20,0,0,Caller),"legacy trademark renders even when game.fnt lacks U+2122 slot");
        Draw(L"WESTWOOD\x0099",20,12);
        CHECK(Lit()>0,"legacy trademark row reaches surface");
        Clear();
        vt::Takeover::BeginLine(font,text,-1,20,12,20,0,0,Caller);
        Draw(text,20,12);
    }
    // Check the engine's Y against independent FreeType bitmap bearings.
    // A font may overhang the old 16-row cell; that is not a baseline shift.
    vt::GlyphSource verticalReference;
    verticalReference.SetAntiAlias(true); verticalReference.SetSupersample(vt::Cfg::Supersample());
    verticalReference.Init(vt::Cfg::FontFile(), vt::Cfg::FontSizeLatin(), vt::Cfg::FontWeight(), 3, 16, vt::Cfg::BaselineRow());
    verticalReference.SetSizes(vt::Cfg::FontSizeLatin(), vt::Cfg::FontSizeCJK());
    char latinPath[MAX_PATH] = {};
    vt::Cfg::ConfigStr("FontFileLatin", "", latinPath, sizeof(latinPath));
    if (latinPath[0]) verticalReference.SetLatinFont(latinPath);
    int firstRow = H, lastRow = -1;
    const int ss = vt::Cfg::Supersample();
    const int hint = vt::Cfg::ConfigInt("Hinting", 0);
    const FT_Int32 rawFlags = (hint == 1 ? FT_LOAD_TARGET_NORMAL : hint == 2 ? FT_LOAD_NO_HINTING : FT_LOAD_TARGET_LIGHT) | FT_LOAD_NO_BITMAP | FT_LOAD_RENDER;
    for (const wchar_t* p = text; *p; ++p) {
        FT_Face face = (FT_Face)verticalReference.FaceHandleFor(*p);
        FT_Set_Transform(face, NULL, NULL);
        CHECK(!FT_Load_Char(face, *p, rawFlags), "reference glyph loads");
        if (!face->glyph->bitmap.rows) continue;
        const int top = 12 + vt::Cfg::BaselineRow() - (face->glyph->bitmap_top + ss - 1) / ss;
        const int bottom = 12 + vt::Cfg::BaselineRow() + ((int)face->glyph->bitmap.rows - face->glyph->bitmap_top + ss - 1) / ss - 1;
        if (top < firstRow) firstRow = top;
        if (bottom > lastRow) lastRow = bottom;
    }
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
        if ((y < firstRow || y > lastRow) && pixels[y * W + x]) { CHECK(false, "engine Y retained with font's natural vertical bearings"); y = H; break; }
    CHECK(fontData == original, "font tables and bitmap data unchanged");
    CHECK(*(unsigned short*)(font + 36) == 0x07FF, "font colour restored/preserved");

    // A fractional centre anchor must keep its measured position while its
    // pixels obey Subpixel=0/1. Compare the complete glyph with an independent
    // zero-phase / fractional-phase raster, rather than just counting pixels.
    verticalReference.SetHinting(hint);
    bool fractionalAnchor = false;
    for (int boxWidth = 120; boxWidth < 124; ++boxWidth)
    {
        Clear();
        const int oldX = 20 + (boxWidth - LegacyWidth(L"i")) / 2;
        CHECK(vt::Takeover::BeginLine(font, L"i", -1, oldX, 12, 20, boxWidth, 1, Caller), "fractional centred glyph prepared");
        vt::Takeover::GetLineInfo(&info);
        if (!(info.originQ & 3)) continue;
        fractionalAnchor = true;
        Draw(L"i", oldX, 12);
        std::vector<unsigned short> expected(W * H, 0);
        const bool subpixel = vt::Cfg::ConfigBool("Subpixel", true);
        const int rasterX = (info.originQ + (subpixel ? 0 : 2)) / 4;
        const int phase = subpixel ? info.originQ % 4 : 0;
        const vt::GlyphCell* glyph = verticalReference.Get('i', -1, phase);
        vt::Target referenceTarget = { expected.data(), W, 0, 0, W - 1, H - 1 };
        CHECK(glyph != NULL, "independent centred glyph exists");
        if (glyph) vt::DrawCellAA(referenceTarget, *glyph, rasterX, 12, 16, 0x07FF, vt::RGB565);
        CHECK(!memcmp(pixels, expected.data(), sizeof(pixels)), "fractional anchor raster honours configured pixel positioning");
        vt::Takeover::GetLineInfo(&before);
        CHECK(before.originQ == info.originQ && before.widthQ == info.widthQ, "pixel snap leaves natural layout and width unchanged");
        break;
    }
    CHECK(fractionalAnchor, "centred regression exercises a fractional pixel origin");

    const wchar_t* backgroundRows[] = { L"就绪", L"Ready", L"11154", L"中文:50%" };
    for (const wchar_t* row : backgroundRows) for (int align = 0; align <= 2; ++align)
    {
        Clear();
        const int anchor = 180, boxX = align == 1 ? 20 : align == 2 ? -140 : 180, boxWidth = 320;
        const int oldX = anchor - (align == 1 ? LegacyWidth(row) / 2 : align == 2 ? LegacyWidth(row) : 0);
        CHECK(vt::Takeover::BeginLine(font,row,-1,oldX,12,boxX,boxWidth,align,Caller),"background reference row prepared");
        vt::Takeover::GetLineInfo(&before);
        vt::Takeover::InkY ink = {};
        CHECK(vt::Takeover::MeasureTextInkY(font,row,anchor,align,&ink),"background ink measured without drawing");
        vt::Takeover::GetLineInfo(&info);
        CHECK(!memcmp(&info,&before,sizeof(info)) && !Lit(),"ink measurement preserves active draw plan and pixels");
        Draw(row,oldX,12);
        int actualTop = H, actualBottom = -1;
        int actualLeft = W, actualRight = -1;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) if (pixels[y * W + x])
        { if (y < actualTop) actualTop = y; if (y + 1 > actualBottom) actualBottom = y + 1;
          if (x < actualLeft) actualLeft = x; if (x + 1 > actualRight) actualRight = x + 1; }
        CHECK(ink.top + 12 == actualTop && ink.bottom + 12 == actualBottom && ink.lines == 1,
              "vertical background edges match complete actual raster at all alignments");
        CHECK(ink.left==actualLeft && ink.right==actualRight,
              "horizontal background edges match actual raster including side bearings at all alignments");
    }

    // Multiline extents must retain intermediate blank rows and native spacing.
    *(int*)(font + 0x1C) = 20;
    vt::Takeover::InkY one = {}, several = {}, crlf = {};
    CHECK(vt::Takeover::MeasureTextInkY(font,L"就绪",20,0,&one) &&
          vt::Takeover::MeasureTextInkY(font,L"就绪\n\n就绪",20,0,&several) &&
          several.lines == 3 && several.top == one.top && several.bottom == one.bottom + 40,
          "vertical ink span retains intentional internal blank line");
    CHECK(vt::Takeover::MeasureTextInkY(font,L"就绪\r\n就绪",20,0,&crlf) &&
          crlf.lines == 2 && crlf.bottom == one.bottom + 20,"CRLF advances exactly one native row");
    CHECK(!vt::Takeover::MeasureTextInkY(font,L"\n就绪",20,0,&several) &&
          !vt::Takeover::MeasureTextInkY(font,L"就绪\n",20,0,&several),"intentional outer blank rows preserve native box");
    const wchar_t* tipRows[] = { L"军械库", L"$800 \u231A00:41 \u26A1-50 gyp" };
    const wchar_t* tipText = L"军械库\n$800 \u231A00:41 \u26A1-50 gyp";
    CHECK(vt::Takeover::MeasureTextInkY(font,tipText,20,0,&several),"mixed tooltip ink includes icons and descenders");
    const int popupY = 10, textY = popupY + 2 - several.top;
    Clear();
    for (int row = 0; row < 2; ++row)
    {
        CHECK(vt::Takeover::BeginLine(font,tipRows[row],-1,20,textY+row*20,20,0,0,Caller),"full tooltip row prepared");
        Draw(tipRows[row],20,textY+row*20);
    }
    const std::vector<unsigned short> fullTooltip(pixels,pixels+W*H);
    Clear();
    *(int*)(font+52) = popupY;
    *(int*)(font+60) = popupY + several.bottom - several.top + 4 - 1;
    for (int row = 0; row < 2; ++row)
    {
        CHECK(vt::Takeover::BeginLine(font,tipRows[row],-1,20,textY+row*20,20,0,0,Caller),"compact tooltip row prepared");
        Draw(tipRows[row],20,textY+row*20);
    }
    CHECK(!memcmp(pixels,fullTooltip.data(),sizeof(pixels)),"compact vertical clip preserves every pixel, including first top and final descenders/icons");
    int firstTooltipInk = H, lastTooltipInk = -1;
    for (int y=0;y<H;++y) for(int x=0;x<W;++x) if(pixels[y*W+x])
    { if(y<firstTooltipInk)firstTooltipInk=y; if(y>lastTooltipInk)lastTooltipInk=y; }
    CHECK(firstTooltipInk==popupY+2 && lastTooltipInk==*(int*)(font+60)-2,"compact tooltip retains 2px above and below actual ink");
    *(int*)(font+52) = 0; *(int*)(font+60) = H-1;
    *(int*)(font + 0x1C) = 0;

    const wchar_t* objectiveRows[] = { L"任务目标一： 保护天气控制机", L"任务目标二： 消灭敌军部队" };
    int objectiveWidths[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        CHECK(vt::Takeover::MeasureDynamicWidth(font, objectiveRows[i], 400, &objectiveWidths[i]), "loading objective row measured");
        Clear();
        CHECK(vt::Takeover::BeginLine(font, objectiveRows[i], -1, 20, 12, 20, objectiveWidths[i], 0, Caller), "objective fits its measured box");
        vt::Takeover::GetLineInfo(&info);
        CHECK(info.tightenedQ == 0 && info.scale1024 == 1024, "measured objective keeps natural spacing and size");
        Draw(objectiveRows[i], 20, 12);
        int lastInk = -1;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
            if (pixels[y * W + x] && x > lastInk) lastInk = x;
        CHECK(lastInk < 20 + objectiveWidths[i] - 2, "actual rendered ink leaves right-side background margin");
    }
    CHECK(vt::Takeover::MeasureDynamicWidth(font,
        L"任务目标一： 保护天气控制机\r\n任务目标二： 消灭敌军部队", 400, &dynamicWidth), "explicit multiline width measured");
    CHECK(dynamicWidth == (objectiveWidths[0] > objectiveWidths[1] ? objectiveWidths[0] : objectiveWidths[1]), "multiline box uses longest explicit row");
    printf("dynamic loading widths: %dpx / %dpx (legacy first row %dpx)\n", objectiveWidths[0], objectiveWidths[1], LegacyWidth(objectiveRows[0]));
    dynamicWidth = 123;
    CHECK(!vt::Takeover::MeasureDynamicWidth(font, objectiveRows[0], 20, &dynamicWidth) && dynamicWidth == 123,
        "automatic wrap falls back atomically without changing dimensions");
    CHECK(!vt::Takeover::MeasureDynamicWidth(font, L"A\tB", 0, &dynamicWidth) && dynamicWidth == 123, "unsupported tabs preserve original measurement");

    Clear(); vt::Takeover::BeginLine(font, L"AV", -1, 20, 12, 20, 0, 0, Caller);
    vt::Takeover::GetLineInfo(&info);
    vt::Takeover::BeginLine(font, L"A", -1, 20, 12, 20, 0, 0, Caller); vt::Takeover::GetLineInfo(&before);
    int aQ = before.widthQ;
    vt::Takeover::BeginLine(font, L"V", -1, 20, 12, 20, 0, 0, Caller); vt::Takeover::GetLineInfo(&before);
    CHECK(info.widthQ < aQ + before.widthQ, "AV pair uses font kerning");

    for (int align = 1; align <= 2; ++align)
    {
        Clear(); const int boxX = 20, width = 160;
        const int oldX = boxX + (align == 1 ? (width - LegacyWidth(text)) / 2 : width - LegacyWidth(text));
        CHECK(vt::Takeover::BeginLine(font, text, -1, oldX, 12, boxX, width, align, Caller), "aligned row prepared");
        vt::Takeover::GetLineInfo(&info);
        CHECK(info.originQ == boxX * 4 + (width * 4 - info.widthQ) / (align == 1 ? 2 : 1), "natural width determines centre/right anchor");
        Draw(text, oldX, 12, 3, 0xF800); // engine decides reveal count/colour
        vt::Takeover::GetLineInfo(&before);
        CHECK(info.widthQ == before.widthQ && info.originQ == before.originQ && before.consumed == 3,
              "partial reveal does not move alignment");
        for (unsigned short p : pixels) CHECK(!(p & 0x07FF), "explicit red keeps only red channel");
    }
    Clear();
    const wchar_t* chinese = L"中文中文中文。";
    vt::Takeover::BeginLine(font, chinese, -1, 20, 12, 20, 0, 0, Caller); vt::Takeover::GetLineInfo(&info);
    CHECK(vt::Takeover::BeginLine(font, chinese, -1, 20, 12, 20, (info.widthQ + 3) / 4 - 1, 0, Caller), "tight box uses existing whitespace");
    CHECK(!vt::Takeover::BeginLine(font, chinese, -1, 20, 12, 20, 1, 0, Caller), "impossible width rejects whole line");
    CHECK(!Lit() && !vt::Takeover::GetLineInfo(&info), "rejected line paints nothing, leaves no active plan");
    CHECK(!vt::Takeover::BeginLine(font, L"ABC\x1", -1, 20, 12, 20, 0, 0, Caller), "unsupported control rejects atomically");
    CHECK(!Lit(), "unsupported suffix leaves no prefix pixels");

    // The power counter starts with U+26A1, absent from the configured Arial
    // face. It used to reject the row and shrink 7/0 separately into 5px cells.
    const unsigned int icons[] = { 0x26A1, 0x26A1, 0x26CF, 0x26CF };
    const wchar_t* counters[] = { L"+1710", L"-1000", L"2/2", L"10/12" };
    std::vector<unsigned short> counterReferences[4];
    for (int sample = 0; sample < 4; ++sample)
    {
    wchar_t decorated[64];
    swprintf(decorated, 64, L"%lc%ls", icons[sample], counters[sample]);
    const wchar_t* power = decorated;
    const int iconAdvance = Advance(icons[sample]);
    Clear();
    CHECK(vt::Takeover::BeginLine(font, counters[sample], -1, 20 + iconAdvance, 12,
                                20 + iconAdvance, 0, 0, Caller), "power digits reference prepared");
    Draw(counters[sample], 20 + iconAdvance, 12);
    std::vector<unsigned short> powerReference(pixels, pixels + W * H);
    const unsigned char* icon = bitmaps + (map[icons[sample]] - 1) * fd.bytes;
    for (int row = 0; row < 16; ++row)
        for (int col = 0; col < icon[0]; ++col)
            if (icon[1 + row * 3 + col / 8] & (0x80 >> (col & 7)))
                powerReference[(12 + row) * W + 20 + col] = 0x07FF;
    Clear();
    CHECK(vt::Takeover::BeginLine(font, power, -1, 20, 12, 20, 168, 0, Caller),
          "native lightning no longer rejects vector digits");
    CHECK(vt::Takeover::GetLineInfo(&info) && info.scale1024 == 1024,
          "power row retains native font sizes");
    CHECK(vt::Takeover::MeasureDynamicWidth(font, power, 168, &dynamicWidth) &&
          dynamicWidth == (info.widthQ + 3) / 4 + 4, "icon and digits share drawing/measurement width");
    Draw(power, 20, 12);
    CHECK(!memcmp(pixels, powerReference.data(), sizeof(pixels)),
          "power pixels equal original lightning plus naturally spaced unscaled digits");
    counterReferences[sample] = powerReference;
    Clear();
    }

    const wchar_t* longRow = L"指挥官，命运和我们开了一个天大的玩笑，我们终于解开了尤里奇袭伦敦之谜——这让西格弗里德都大为震惊。";
    CHECK(vt::Takeover::BeginLine(font, longRow, -1, 4, 12, 4, 716, 0, Caller), "long row fits known width");
    vt::Takeover::GetLineInfo(&info);
    printf("long row: width=%d/4px box=%dpx scale=%d/1024 tightened=%d/4px\n", info.widthQ, info.boxWidth, info.scale1024, info.tightenedQ);
    CHECK(info.scale1024 >= 922 && info.scale1024 <= 1024 && info.widthQ <= 716 * 4,
          "long row fits by whitespace or one uniform scale within 90..100 percent");
    // The Chinese line's whitespace depends on its font, so use wide Latin
    // outlines to exercise scaling independently of that font-specific case.
    const wchar_t* wideRow = L"WWWWWWWWWWWWWWWWWWWW";
    CHECK(vt::Takeover::BeginLine(font, wideRow, -1, 4, 12, 4, 0, 0, Caller), "wide line measured");
    vt::Takeover::GetLineInfo(&info);
    const int tightWidth = info.widthQ * 95 / 400;
    CHECK(vt::Takeover::BeginLine(font, wideRow, -1, 4, 12, 4, tightWidth, 0, Caller), "wide line fits by uniform scale");
    vt::Takeover::GetLineInfo(&info);
    CHECK(info.scale1024 >= 922 && info.scale1024 < 1024 && info.widthQ <= tightWidth * 4,
          "dense line exercises uniform scale when whitespace is insufficient");
    Clear();

    Clear();
    CHECK(vt::Takeover::BeginLine(font, L"中MIDAS文", -1, 20, 12, 20, 0, 0, Caller), "Chinese/Latin word transitions prepared");
    vt::Takeover::GetLineInfo(&info);
    CHECK(info.mixedAddedQ > 0 && info.boxWidth == W - 20, "mixed ink gaps and real clip width used");
    vt::Takeover::BeginLine(font, L"中 MIDAS 文", -1, 20, 12, 20, 0, 0, Caller);
    vt::Takeover::GetLineInfo(&info); CHECK(info.mixedAddedQ == 0, "explicit spaces do not receive duplicate mixed spacing");
    vt::Takeover::BeginLine(font, L"中，MIDAS", -1, 20, 12, 20, 0, 0, Caller);
    vt::Takeover::GetLineInfo(&info); CHECK(info.mixedAddedQ == 0, "punctuation separates scripts without an extra word gap");
    *(int*)(font + 56) = 80;
    CHECK(vt::Takeover::BeginStringLine(font, L"A", -1, 20, 12), "ordinary string uses its actual clip box");
    vt::Takeover::GetLineInfo(&info); CHECK(info.boxWidth == 61 && info.widthQ <= 61 * 4, "ordinary row has a bounded available width");
    *(int*)(font + 56) = W - 1;
    const wchar_t* hinted = L"AV";
    int hintX = 20 + (160 - LegacyWidth(hinted)) / 2;
    vt::Takeover::SetLineBox(font, hinted, hintX, 12, 20, 160, 1);
    CHECK(vt::Takeover::BeginStringLine(font, hinted, -1, hintX, 12), "viewport hint passed into DrawString");
    vt::Takeover::GetLineInfo(&info);
    CHECK(info.boxWidth == 160 && info.originQ == 80 + (640 - info.widthQ) / 2, "ordinary string re-centres around original anchor using natural width");
    vt::Takeover::EndLine(font);
    vt::Takeover::BeginStringLine(font, hinted, -1, hintX, 12); vt::Takeover::GetLineInfo(&info);
    CHECK(info.boxWidth == W - hintX, "consumed viewport hint does not leak into another string");

    Clear();
    CHECK(vt::Takeover::BeginLine(font, L"A\tB", -1, 20, 12, 20, 0, 0, Caller), "tab line prepared");
    int end;
    CHECK(vt::Takeover::TryLineBlit(font, 'A', 20, 12, -1, Caller, &end), "tab prefix");
    int tabEnd = end + 32 - ((end + 32) % 32);
    CHECK(vt::Takeover::TryLineBlit(font, '\t', end, 12, -1, Caller, &end) && end == tabEnd, "legacy tab return preserved");
    CHECK(vt::Takeover::TryLineBlit(font, 'B', end, 12, -1, Caller, &end), "tab suffix");
    vt::Takeover::EndLine(font); CHECK(!vt::Takeover::GetLineInfo(&info), "unlock clears context");
    CHECK(!vt::Takeover::BeginLine(font, L"A\tB", -1, 20, 12, 20, 160, 1, Caller), "aligned tabs fall back without guessing tab grid");
    void* base = *(void**)(font + 12); *(void**)(font + 12) = NULL;
    CHECK(!vt::Takeover::BeginLine(font, text, -1, 20, 12, 20, 0, 0, Caller), "unlocked surface falls back");
    *(void**)(font + 12) = base;

    Clear(); vt::Takeover::BeginLine(font, text, -1, 20, 12, 20, 0, 0, Caller);
    vt::Takeover::GetLineInfo(&before);
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, ThreadProbe, NULL, 0, NULL);
    CHECK(thread != NULL, "TLS worker created");
    if (thread) { WaitForSingleObject(thread, 10000); DWORD exit; GetExitCodeThread(thread, &exit); CHECK(exit == 0, "TLS has no cross-thread context"); CloseHandle(thread); }
    vt::Takeover::GetLineInfo(&info); CHECK(info.originQ == before.originQ && info.count == before.count, "worker does not replace parent line");

    Clear(); vt::Takeover::BeginLine(font, text, -1, 20, 12, 20, 0, 0, Caller); Draw(text, 20, 12);
    std::vector<unsigned short> reference(pixels, pixels + W * H);
    DWORD start = GetTickCount();
    for (int i = 0; i < 2000; ++i)
    {
        Clear(); CHECK(vt::Takeover::BeginLine(font, text, -1, 20, 12, 20, 0, 0, Caller), "repeat preparation");
        Draw(text, 20, 12);
    }
    printf("2000 warm line draws: %lu ms\n", GetTickCount() - start);
    CHECK(!memcmp(pixels, reference.data(), sizeof(pixels)), "repeated rows have no phase drift or accumulated spacing");
    Clear(); vt::Takeover::BeginLine(font, text, -1, -10, 12, -10, 0, 0, Caller); Draw(text, -10, 12);
    CHECK(Lit() > 0, "negative origin clips safely at surface left edge");

    // Exercise the DLL hook adapters using Syringe's register-block layout.
    HMODULE dll = LoadLibraryA("VectorText.dll");
    CHECK(dll != NULL, "installed DLL loads");
    CHECK(GetModuleHandleA("Phobos.dll") == NULL && GetModuleHandleA("Ares.dll") == NULL,
          "installed DLL loads without pulling in Phobos or Ares");
    if (dll)
    {
        typedef DWORD(__cdecl* Hook)(REGISTERS*);
        Hook blit = (Hook)GetProcAddress(dll, "VT_Hook_BitFont_Blit");
        Hook unlock = (Hook)GetProcAddress(dll, "VT_Hook_BitFont_Unlock");
        Hook boxHook = (Hook)GetProcAddress(dll, "VT_Hook_Drawing_LineBox");
        const char* entries[] = { "VT_Hook_BitFont_DrawString", "VT_Hook_BitText_LineBreak", "VT_Hook_BitText_LineWrap", "VT_Hook_BitText_LineLast" };
        const DWORD callers[] = { 0x43464D, 0x434EA6, 0x4350E1, 0x4352BA };
        CHECK(blit && unlock, "Blit/Unlock exports exist");
        CHECK(boxHook != NULL, "viewport hook exported");
        Hook dimensionsDone = (Hook)GetProcAddress(dll, "VT_Hook_BitFont_DimensionDone");
        dimension_machine::Check(dll, font);
        dimension_machine::CheckBackground(dll, font);
        Hook dimensionsEntry = (Hook)GetProcAddress(dll, "VT_Hook_BitFont_GetTextDimension");
        CHECK(dimensionsDone != NULL, "dynamic width epilogue hook exported");
        if (dimensionsDone && dimensionsEntry)
        {
            DWORD stack[32] = {};
            int result = 190, height = 40;
            stack[8] = 0x553199; stack[2] = (DWORD)font;
            stack[10] = (DWORD)&result; stack[11] = (DWORD)&height; stack[12] = 400;
            REGISTERS r = {}; r.esp = (DWORD)stack; r.eax = 0xDEADBEEF;
            auto finishMeasurement = [&](const wchar_t* input) {
                stack[9] = (DWORD)input;
                r.esp = (DWORD)(stack + 8); r.ecx = (DWORD)font;
                CHECK(dimensionsEntry(&r) == 0 && r.esp == (DWORD)(stack + 8), "measurement entry leaves original call frame intact");
                // Actual engine code at 433E45 writes its accumulated width
                // to entry ESP+4, destroying the text-pointer argument.
                stack[9] = (DWORD)result;
                r.esp = (DWORD)stack;
                return dimensionsDone(&r);
            };
            const wchar_t* objectives = L"任务目标一： 保护天气控制机\n任务目标二： 消灭敌军部队";
            const int expectedObjectives = objectiveWidths[0] > objectiveWidths[1] ? objectiveWidths[0] : objectiveWidths[1];
            CHECK(finishMeasurement(objectives) == 0 && result == expectedObjectives,
                "loading hook survives actual text argument overwritten with width");
            CHECK(result > 190 && height == 40 && r.eax == 0xDEADBEEF && r.esp == (DWORD)stack,
                "loading hook widens old box, preserves height, return registers and stack");
            result = 190; stack[8] = 0x4A5EF1;
            CHECK(finishMeasurement(objectives) == 0 && result == 190, "ordinary layout measurement remains untouched");
            result = 190; stack[8] = 0x433EE6; stack[13] = 0x623A81; stack[12] = 0;
            int expectedWidth = 0; vt::Takeover::MeasureDynamicWidth(font, text, 0, &expectedWidth);
            CHECK(finishMeasurement(text) == 0 && result == expectedWidth && height == 40,
                "message background survives overwritten text argument and gets natural width");
            result = 190;
            CHECK(dimensionsDone(&r) == 0 && result == 190, "saved measurement is consumed once");
            result = 190; stack[13] = 0x5D4706;
            CHECK(finishMeasurement(text) == 0 && result == 190, "message wrapping probe retains legacy metrics");
            result = 190; stack[13] = 0x623A81;
            CHECK(finishMeasurement((const wchar_t*)1) == 0 && result == 190, "invalid string falls back without changing output");
            result = 190; stack[8] = 0x5531EF; stack[12] = 400;
            CHECK(finishMeasurement(objectives) == 0 && result == expectedObjectives, "second loading box measurement also survives argument mutation");
            stack[9] = (DWORD)objectives; r.ecx = (DWORD)font; r.esp = (DWORD)(stack + 8);
            dimensionsEntry(&r); result = 190; stack[9] = 190;
            r.esp = (DWORD)(stack + 1);
            CHECK(dimensionsDone(&r) == 0 && result == 190, "saved width cannot leak into another stack frame");
            stack[8] = 0x4A5EF1;
            CHECK(finishMeasurement(text) == 0 && result == 190, "aborted capture is cleared by the next measurement");
        }
        for (int i = 0; i < 4 && blit && unlock; ++i)
        {
            Hook entry = (Hook)GetProcAddress(dll, entries[i]); CHECK(entry != NULL, entries[i]); if (!entry) continue;
            DWORD stack[40] = {}; const wchar_t* s = L"A";
            stack[1] = (DWORD) s; stack[2] = 20; stack[3] = 12;
            stack[0x44 / 4] = (DWORD)font; stack[0x4C / 4] = (DWORD)(s + 1);
            stack[0x50 / 4] = 20; stack[0x54 / 4] = 12; stack[0x58 / 4] = 160;
            REGISTERS r = {}; r.ecx = (DWORD)font; r.esp = (DWORD)stack; r.ebp = (DWORD)s;
            memset(pixels, 0, sizeof(pixels));
            CHECK(entry(&r) == 0 && r.esp == (DWORD)stack, "row adapter replays original instructions, preserves ESP");
            DWORD call[5] = { callers[i], 'A', 20, 12, (DWORD)-1 };
            r.esp = (DWORD)call;
            DWORD trampoline = blit(&r);
            CHECK(trampoline != 0 && r.eax == (DWORD)(20 + Advance('A')) && Lit() > 0, "hook draws planned line, returns legacy tracking");
            CHECK(r.esp == (DWORD)call, "Syringe popad ESP contract retained");
            const unsigned char expected[] = { 0x8B,0x14,0x24,0x83,0xC4,0x14,0x52,0xC3 };
            CHECK(trampoline && !memcmp((void*)trampoline, expected, sizeof(expected)), "ret 10h trampoline machine code retained");
            CHECK(unlock(&r) == 0, "unlock adapter passes through");
            // Per-glyph fallback must agree with native Blit's width + tracking,
            // just as the planned row does. The old assertion omitted +2C.
            r.eax = 0; CHECK(blit(&r) != 0 && r.eax == (DWORD)(20 + Advance('A')), "per-glyph fallback after unlock preserves native tracking");
        }
        Hook core = (Hook)GetProcAddress(dll, entries[0]);
        if (blit && unlock && core)
        {
            const int failuresBefore = failures;
            for (int sample = 0; sample < 4; ++sample)
            {
                wchar_t decorated[64];
                swprintf(decorated, 64, L"%lc%ls", icons[sample], counters[sample]);
                memset(pixels, 0, sizeof(pixels));
                DWORD coreStack[6] = { 0x434BCF, (DWORD)decorated, 20, 12, 0, 0 };
                REGISTERS r = {}; r.ecx = (DWORD)font; r.esp = (DWORD)coreStack;
                CHECK(core(&r) == 0 && r.esp == (DWORD)coreStack,
                      "counter DrawString hook preserves original stack without Phobos");
                int oldX = 20;
                for (const wchar_t* p = decorated; *p; ++p)
                {
                    DWORD call[5] = { Caller, (DWORD)*p, (DWORD)oldX, 12, (DWORD)-1 };
                    r.esp = (DWORD)call;
                    CHECK(blit(&r) != 0 && r.eax == (DWORD)(oldX + Advance(*p)),
                          "counter Blit hook returns legacy advance without Phobos");
                    CHECK(r.esp == (DWORD)call, "counter Blit hook preserves Syringe ESP");
                    oldX = (int)r.eax;
                }
                CHECK(!memcmp(pixels, counterReferences[sample].data(), sizeof(pixels)),
                      "installed DLL retains native icon and natural digits without Phobos");
                CHECK(unlock(&r) == 0, "counter unlock passes through without Phobos");
            }
            CHECK(GetModuleHandleA("Phobos.dll") == NULL && GetModuleHandleA("Ares.dll") == NULL,
                  "render and measurement hooks never load Phobos or Ares");
            printf("no-Phobos/Ares DLL counter hooks: %s (4 cases)\n",
                   failures == failuresBefore ? "PASS" : "FAIL");
        }
        if (blit && unlock && boxHook && core)
        {
            const wchar_t* s = L"A";
            int oldX = 20 + (160 - LegacyWidth(s)) / 2;
            Clear(); vt::Takeover::SetLineBox(font, s, oldX, 12, 20, 160, 1);
            vt::Takeover::BeginStringLine(font, s, -1, oldX, 12); Draw(s, oldX, 12);
            std::vector<unsigned short> expected(pixels, pixels + W * H);
            memset(pixels, 0, sizeof(pixels));
            int rect[4] = { 20, 0, 160, H };
            DWORD boxStack[32] = {}; boxStack[0x10 / 4] = (DWORD)s;
            boxStack[0x44 / 4] = LegacyWidth(s); boxStack[0x50 / 4] = 0x100;
            REGISTERS r = {}; r.ebx = (DWORD)font; r.edi = (DWORD)rect;
            r.esi = oldX; r.ebp = 12; r.esp = (DWORD)boxStack;
            CHECK(boxHook(&r) == 0, "viewport adapter replays original instruction");
            DWORD coreStack[6] = { 0x434BCF, (DWORD)s, (DWORD)oldX, 12, 0, 0 };
            r.ecx = (DWORD)font; r.esp = (DWORD)coreStack;
            core(&r);
            DWORD call[5] = { Caller, 'A', (DWORD)oldX, 12, (DWORD)-1 }; r.esp = (DWORD)call;
            CHECK(blit(&r) != 0 && !memcmp(pixels, expected.data(), sizeof(pixels)), "viewport-to-core hooks draw at exact natural centre");
            unlock(&r);
        }
        FreeLibrary(dll);
    }
    CHECK(fontData == original, "all hooks preserve original font tables");
    printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
