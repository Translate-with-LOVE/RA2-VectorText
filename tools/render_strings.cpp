// ===========================================================================
//  render_strings.cpp -- renders the strings the game *actually* drew (read
//  from a VectorText.log) through the real M1 takeover path and reports the
//  glyph coverage the in-game run will produce.
//
//  Per string, two rows are drawn into a synthetic 16-bit surface:
//      row 1 : the original game.fnt bitmap glyphs (what the game draws today)
//      row 2 : our vector glyphs, via Takeover::TryBlit (the hooked path)
//
//  usage: render_strings.exe [--log <VectorText.log>] [--top N] [--min-count N]
//                            [--out sheet.bmp] [--width px] [--aa 0|1]
// ===========================================================================

#include "../src/Takeover.h"
#include "../src/GlyphSource.h"
#include "../src/PixelWriter.h"
#include "../src/Logger.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wchar.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

// ------------------------------------------------------------- game.fnt ----
static unsigned char* g_fnt = NULL;
static int g_lines = 16, g_symbolSize = 49;
static unsigned short* g_map = NULL;
static const unsigned char* g_bitmaps = NULL;

static bool LoadGameFnt(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
    g_fnt = (unsigned char*)malloc(size);
    if (!g_fnt || fread(g_fnt, 1, size, f) != (size_t)size) { fclose(f); return false; }
    fclose(f);
    if (*(unsigned int*)g_fnt != 0x546E6F66u) return false;
    const unsigned int* hdr = (const unsigned int*)(g_fnt + 4);
    g_lines = (int)hdr[2];
    g_symbolSize = (int)hdr[5];
    g_map = (unsigned short*)(g_fnt + 0x1C);
    g_bitmaps = g_fnt + 0x1C + 0x20000;
    return true;
}

static int GameAdvance(unsigned int cp)
{
    if (cp >= 0x10000 || !g_map[cp]) return -1;
    return g_bitmaps[(size_t)(g_map[cp] - 1) * g_symbolSize];
}

static bool GameCell(unsigned int cp, vt::GlyphCell* out)
{
    if (cp >= 0x10000 || !g_map[cp]) return false;
    const unsigned char* p = g_bitmaps + (size_t)(g_map[cp] - 1) * g_symbolSize;
    memset(out, 0, sizeof(*out));
    out->width = p[0];
    memcpy(out->bits, p + 1, 3 * g_lines);
    for (int y = 0; y < g_lines; ++y)
        for (int x = 0; x < 24; ++x)
            if (out->bits[y * 3 + (x >> 3)] & (0x80 >> (x & 7)))
                out->cov[y * 24 + x] = 255;
    return true;
}

// -------------------------------------------------- synthetic engine objects
#pragma pack(push, 1)
struct FakeInternal
{
    unsigned int  unknown00;
    unsigned int  stride, lines, unknown0C, unknown10, symbolBytes;
    const unsigned short* symTable;
    const unsigned char*  bitmaps;
};
#pragma pack(pop)

static unsigned char g_bitFont[0x80];
static const int WIDTH = 720;
static int g_usedW = 720;
static int g_height = 64;
static unsigned short* g_surf = NULL;
static const unsigned short BG   = 0x0000;
static const unsigned short OURS = 0xC618;      // RGB565 light grey
static const unsigned short ORIG = 0x7BEF;      // dimmer: the original row

static void WriteBmp(const char* path)
{
    const int rowBytes = g_usedW * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const int dataSize = (rowBytes + pad) * g_height;
    const int fileSize = 54 + dataSize;
    unsigned char* img = (unsigned char*)malloc(fileSize);
    if (!img) return;
    memset(img, 0, 54);
    img[0] = 'B'; img[1] = 'M';
    *(int*)(img + 2) = fileSize; *(int*)(img + 10) = 54; *(int*)(img + 14) = 40;
    *(int*)(img + 18) = g_usedW; *(int*)(img + 22) = g_height;
    *(short*)(img + 26) = 1; *(short*)(img + 28) = 24;
    *(int*)(img + 34) = dataSize;
    for (int y = 0; y < g_height; ++y)
    {
        unsigned char* dst = img + 54 + (size_t)(g_height - 1 - y) * (rowBytes + pad);
        for (int x = 0; x < g_usedW; ++x)
        {
            const unsigned short c = g_surf[y * WIDTH + x];
            dst[x * 3 + 0] = (unsigned char)((c & 0x1F) * 255 / 31);
            dst[x * 3 + 1] = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
            dst[x * 3 + 2] = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
        }
    }
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(img, 1, fileSize, f); fclose(f); }
    free(img);
}

// ------------------------------------------------------------ log parsing --
static std::string unescape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            const char c = s[i + 1];
            if (c == 'n' || c == 't') { out += ' '; ++i; continue; }   // one-line sheet
            if (c == 'r') { ++i; continue; }
            if (c == '\\') { out += '\\'; ++i; continue; }
            if (c == '"') { out += '"'; ++i; continue; }
            if (c == 'x' && i + 3 < s.size())
            {
                char hex[3] = { s[i + 2], s[i + 3], 0 };
                out += (char)strtol(hex, NULL, 16);
                i += 3;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string WideToUtf8(unsigned int cp)
{
    const wchar_t w[2] = { (wchar_t)cp, 0 };
    char buf[8] = { 0 };
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, 1, buf, sizeof(buf) - 1, NULL, NULL);
    return std::string(buf, n > 0 ? n : 0);
}

struct Entry { unsigned long long count; std::string text; };

static void ParseLog(const char* path, std::map<std::string, unsigned long long>& out, long long* totalLines)
{
    FILE* f = fopen(path, "rb");
    if (!f) return;
    std::string line;
    int ch;
    while ((ch = fgetc(f)) != EOF)
    {
        if (ch != '\n') { line += (char)ch; continue; }
        ++*totalLines;
        if (line.compare(0, 4, "UNIQ") == 0)
        {
            const size_t x = line.find(" x");
            const size_t q1 = line.find('"');
            const size_t q2 = line.rfind('"');
            if (x != std::string::npos && q1 != std::string::npos && q2 > q1)
                out[unescape(line.substr(q1 + 1, q2 - q1 - 1))] += _strtoui64(line.c_str() + x + 2, NULL, 10);
        }
        line.clear();
    }
    fclose(f);
}

// ------------------------------------------------------------------ main ---
int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    const char* logPath = "..\\..\\..\\VectorText.log";
    const char* fntPath = "..\\..\\..\\game.fnt";
    const char* outPath = "strings_sheet.bmp";
    int top = 40, minCount = 2, aa = 1, maxWidth = WIDTH - 8;
    int wght = 400, darkening = 0, subpixel = 1; double gamma = 1.0; const char* metrics = "scaled"; double advScale = 1.05;

    for (int i = 1; i < argc - 1; ++i)
    {
        if (!strcmp(argv[i], "--log"))            logPath = argv[++i];
        else if (!strcmp(argv[i], "--fnt"))       fntPath = argv[++i];
        else if (!strcmp(argv[i], "--out"))       outPath = argv[++i];
        else if (!strcmp(argv[i], "--top"))       top = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min-count")) minCount = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aa"))        aa = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width"))     maxWidth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wght"))      wght = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--darkening")) darkening = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gamma"))     gamma = atof(argv[++i]);
        else if (!strcmp(argv[i], "--subpixel"))  subpixel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--metrics"))   metrics = argv[++i];
        else if (!strcmp(argv[i], "--advscale"))  advScale = atof(argv[++i]);
    }

    // the tool loads the same INI the DLL would
    FILE* ini = fopen("VectorText.ini", "w");
    if (ini)
    {
        fprintf(ini, "[VectorText]\nEnabled=1\nMode=aa\nAntiAlias=%d\n"
                     "FontFile=C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf\n"
                     "FontWeight=%d\nStemDarkening=%d\nGamma=%.2f\nSubpixel=%d\n"
                     "FontSizeLatin=13\nFontSizeCJK=16\nBaselineRow=13\nFitToAdvance=1\n"
                     "Metrics=%s\nAdvanceScale=%.2f\n",
                aa ? 1 : 0, wght, darkening, gamma, subpixel, metrics, advScale);
        fclose(ini);
    }

    if (!LoadGameFnt(fntPath)) { printf("[x] cannot load %s\n", fntPath); return 1; }
    if (!vt::Takeover::Init()) { printf("[x] Takeover::Init failed (font missing?)\n"); return 2; }

    std::map<std::string, unsigned long long> byText;
    long long logLines = 0;
    ParseLog(logPath, byText, &logLines);
    printf("[*] log      : %s (%lld lines, %d distinct strings)\n", logPath, logLines, (int)byText.size());

    std::vector<Entry> sel;
    for (std::map<std::string, unsigned long long>::iterator it = byText.begin(); it != byText.end(); ++it)
        if ((int)it->second >= minCount && !it->first.empty())
        {
            Entry e; e.count = it->second; e.text = it->first;
            sel.push_back(e);
        }
    std::sort(sel.begin(), sel.end(), [](const Entry& a, const Entry& b) { return a.count > b.count; });
    if ((int)sel.size() > top) sel.resize(top);
    if (sel.empty()) { printf("[x] no strings matched\n"); return 3; }

    g_height = (int)sel.size() * 40 + 8;
    g_surf = (unsigned short*)malloc(sizeof(unsigned short) * WIDTH * g_height);
    if (!g_surf) return 4;
    for (int i = 0; i < WIDTH * g_height; ++i) g_surf[i] = BG;

    FakeInternal in;
    memset(&in, 0, sizeof(in));
    in.stride = 3; in.lines = g_lines; in.symbolBytes = (unsigned int)g_symbolSize;
    in.symTable = g_map; in.bitmaps = g_bitmaps;

    memset(g_bitFont, 0, sizeof(g_bitFont));
    *(void**)(g_bitFont + 0x04) = &in;
    *(void**)(g_bitFont + 0x0C) = g_surf;
    *(int*)(g_bitFont + 0x10) = WIDTH;
    *(unsigned short*)(g_bitFont + 0x24) = OURS;

    vt::Target t;
    t.base = g_surf; t.pitch = WIDTH; t.clipL = 0; t.clipR = WIDTH - 1;

    // ink = sum of coverage in a row band (counts partial pixels too, which is
    // exactly what the eye integrates when judging stroke weight)
    auto InkIn = [](int yTop) -> int {
        int sum = 0;
        for (int yy = yTop; yy < yTop + 20 && yy < g_height; ++yy)
            for (int xx = 0; xx < WIDTH; ++xx)
                if (g_surf[yy * WIDTH + xx]) ++sum;
        return sum;
    };

    unsigned long long nChars = 0, nDrawn = 0, nRefused = 0;
    long long inkOrig = 0, inkOurs = 0;
    std::map<unsigned int, unsigned long long> used, missFnt, missFT;
    int widest = 8;

    int y = 4;
    for (size_t i = 0; i < sel.size(); ++i)
    {
        const std::wstring w = Utf8ToWide(sel[i].text);

        // ---- row 1: the original bitmap glyphs (dim) --------------------
        t.clipT = y; t.clipB = y + g_lines - 1;
        const int inkBeforeOrig = 0;
        int inc0 = InkIn(y);
        {
            int x = 4;
            for (size_t k = 0; k < w.size(); ++k)
            {
                vt::GlyphCell gc;
                if (!GameCell((unsigned int)w[k], &gc)) continue;
                if (x + gc.width > maxWidth) break;
                vt::DrawCell(t, gc, x, y, g_lines, ORIG);
                x += gc.width;
            }
            if (x > widest) widest = x;
        }
        inkOrig += InkIn(y) - inc0;

        // ---- row 2: our glyphs, through the real takeover path ----------
        y += 20;
        int inc1 = InkIn(y);
        *(int*)(g_bitFont + 0x30) = 0;
        *(int*)(g_bitFont + 0x34) = y;
        *(int*)(g_bitFont + 0x38) = WIDTH - 1;
        *(int*)(g_bitFont + 0x3C) = y + g_lines - 1;
        {
            int x = 4;
            for (size_t k = 0; k < w.size(); ++k)
            {
                const unsigned int cp = (unsigned int)w[k];
                ++nChars;
                ++used[cp];
                if (x > maxWidth)
                    break;

                int newX = -1;
                const bool viaAA = (vt::Cfg::Mode() == vt::Cfg::Mode_AA);
                const bool ok = viaAA ? vt::Takeover::DrawAA(g_bitFont, cp, x, y, -1)
                                      : vt::Takeover::TryBlit(g_bitFont, cp, x, y, -1, &newX);
                if (ok)
                {
                    ++nDrawn;
                    x = newX;
                }
                else
                {
                    ++nRefused;
                    if (GameAdvance(cp) < 0) ++missFnt[cp];
                    else                     ++missFT[cp];
                    x += (GameAdvance(cp) > 0 ? GameAdvance(cp) : 6);
                }
            }
            if (x > widest) widest = x;
        }
        inkOurs += InkIn(y) - inc1;
        y += 20;
    }

    g_usedW = widest + 8 > WIDTH ? WIDTH : widest + 8;
    WriteBmp(outPath);

    // ------------------------------------------------------------- report --
    printf("[*] rendered : %d strings, %llu characters\n", (int)sel.size(), nChars);
    printf("[*] takeover : drawn=%llu (%.2f%%)  refused=%llu (%.2f%%)\n",
           nDrawn, nChars ? 100.0 * nDrawn / nChars : 0.0,
           nRefused, nChars ? 100.0 * nRefused / nChars : 0.0);
    printf("[*] ink      : original=%lld ours=%lld  ratio=%.3f  (wght=%d darkening=%d gamma=%.2f)\n",
           inkOrig, inkOurs, inkOrig ? (double)inkOurs / (double)inkOrig : 0.0, wght, darkening, gamma);
    printf("[*] sheet    : %s (%dx%d)\n", outPath, g_usedW, g_height);

    printf("\n[!] characters with NO glyph in game.fnt (engine draws its own placeholder,\n"
           "    we hand the glyph back -> these show up as unknownGlyph in the log):\n");
    if (missFnt.empty())
        printf("    none -- every character used has a game.fnt glyph\n");
    else
    {
        std::vector<std::pair<unsigned long long, unsigned int> > v;
        for (std::map<unsigned int, unsigned long long>::iterator it = missFnt.begin(); it != missFnt.end(); ++it)
            v.push_back(std::make_pair(it->second, it->first));
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 20; ++i)
            printf("    U+%04X  '%s'  x%llu\n", v[i].second, WideToUtf8(v[i].second).c_str(), v[i].first);
    }

    printf("\n[!] characters the vector font has no glyph for (FreeType fails -> engine draws):\n");
    if (missFT.empty())
        printf("    none\n");
    else
    {
        std::vector<std::pair<unsigned long long, unsigned int> > v;
        for (std::map<unsigned int, unsigned long long>::iterator it = missFT.begin(); it != missFT.end(); ++it)
            v.push_back(std::make_pair(it->second, it->first));
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 20; ++i)
            printf("    U+%04X  '%s'  x%llu\n", v[i].second, WideToUtf8(v[i].second).c_str(), v[i].first);
    }

    free(g_surf);
    return 0;
}
