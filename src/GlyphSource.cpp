#include "GlyphSource.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_MODULE_H

#include <string.h>
#include <stdlib.h>

namespace vt
{
    GlyphSource::GlyphSource()
        : m_lib(NULL), m_faceA(NULL), m_faceB(NULL), m_size(13),
          m_sizeLatin(13), m_sizeCJK(16), m_cjkFrom(0x2E80u),
          m_stride(3), m_lines(16), m_baseline(13), m_fit(true), m_aa(false), m_darkening(0), m_ss(1), m_fitMode(1), m_vertFill(false), m_darkErr{0,0,0}, m_classAlign(true),
          m_path(NULL), m_weight(400), m_csInit(false)
    {
        InitializeCriticalSection(&m_cs);
        m_csInit = true;
    }

    GlyphSource::~GlyphSource()
    {
        Shutdown();
        if (m_csInit)
        {
            DeleteCriticalSection(&m_cs);
            m_csInit = false;
        }
    }

    void GlyphSource::CloseFace(void** face)
    {
        if (*face)
        {
            FT_Done_Face((FT_Face)*face);
            *face = NULL;
        }
    }

    void* GlyphSource::OpenFace(int pixelSize)
    {
        if (!m_lib || !m_path)
            return NULL;

        FT_Face face = NULL;
        if (FT_New_Face((FT_Library)m_lib, m_path, 0, &face))
            return NULL;

        // Variable fonts: the default instance is often not the weight we want
        // (NotoSerifSC-VF defaults to ExtraLight 200).
        if (m_weight > 0 && FT_HAS_MULTIPLE_MASTERS(face))
        {
            FT_MM_Var* mm = NULL;
            if (!FT_Get_MM_Var(face, &mm))
            {
                FT_Fixed* coords = (FT_Fixed*)malloc(sizeof(FT_Fixed) * mm->num_axis);
                if (coords)
                {
                    for (FT_UInt a = 0; a < mm->num_axis; ++a)
                    {
                        coords[a] = mm->axis[a].def;
                        const unsigned int t = mm->axis[a].tag;
                        char tag[5] = { (char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t, 0 };
                        if (!strcmp(tag, "wght"))
                            coords[a] = m_weight << 16;
                    }
                    FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords);
                    free(coords);
                }
                FT_Done_MM_Var((FT_Library)m_lib, mm);
            }
        }

        if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixelSize))
        {
            FT_Done_Face(face);
            return NULL;
        }
        return face;
    }

    bool GlyphSource::Init(const char* ttfPath, int pixelSize, int weight,
                           int strideBytes, int lines, int baselineRow)
    {
        Shutdown();

        m_stride   = strideBytes;
        m_lines    = lines;
        m_baseline = baselineRow;
        m_size     = pixelSize;
        m_sizeLatin = pixelSize;
        m_sizeCJK   = pixelSize;
        m_path     = ttfPath;
        m_weight   = weight;

        FT_Library lib = NULL;
        if (FT_Init_FreeType(&lib))
            return false;
        m_lib = lib;

        // stem darkening must be set before the faces are created
        if (m_darkening > 0)
        {
            // The documented knobs are module specific: CFF exposes
            // "no-stem-darkening" and the auto-hinter "darkening"/"warping".
            // Try them and RECORD what the library answered (the previous code
            // used a property name that does not exist and ignored the error).
            FT_UInt amount = (FT_UInt)m_darkening;
            FT_Bool noDark = 0;
            FT_Error e1 = FT_Property_Set(lib, "cff", "no-stem-darkening", &noDark);
            FT_Error e2 = FT_Property_Set(lib, "autofitter", "darkening", &amount);
            FT_Error e3 = FT_Property_Set(lib, "truetype", "darkening", &amount);
            m_darkErr[0] = (int)e1; m_darkErr[1] = (int)e2; m_darkErr[2] = (int)e3;
        }

        m_ss = (m_ss < 1) ? 1 : (m_ss > 4 ? 4 : m_ss);
        m_faceA = OpenFace(pixelSize * m_ss);
        if (!m_faceA)
        {
            FT_Done_FreeType(lib);
            m_lib = NULL;
            return false;
        }

        if (m_csInit)
        {
            EnterCriticalSection(&m_cs);
            m_cache.clear();
            LeaveCriticalSection(&m_cs);
        }
        return true;
    }

    void GlyphSource::Shutdown()
    {
        CloseFace(&m_faceB);
        CloseFace(&m_faceA);
        if (m_lib)
        {
            FT_Done_FreeType((FT_Library)m_lib);
            m_lib = NULL;
        }
        if (m_csInit)
        {
            EnterCriticalSection(&m_cs);
            m_cache.clear();
            LeaveCriticalSection(&m_cs);
        }
    }

    void GlyphSource::SetSupersample(int ss)
    {
        if (ss < 1) ss = 1;
        if (ss > 4) ss = 4;
        const int old = m_ss;
        m_ss = ss;
        if (!m_faceA || old == ss)
            return;
        // sizes are stored in target pixels, so the faces must be rebuilt
        CloseFace(&m_faceB);
        if (FT_Set_Pixel_Sizes((FT_Face)m_faceA, 0, (FT_UInt)(m_sizeLatin * m_ss)))
            return;
        if (m_sizeCJK != m_sizeLatin)
            m_faceB = OpenFace(m_sizeCJK * m_ss);
        if (m_csInit)
        {
            EnterCriticalSection(&m_cs);
            m_cache.clear();
            LeaveCriticalSection(&m_cs);
        }
    }

    void GlyphSource::SetAntiAlias(bool on)
    {
        if (m_csInit)
        {
            EnterCriticalSection(&m_cs);
            m_aa = on;
            m_cache.clear();                 // coverage is baked into the cell
            LeaveCriticalSection(&m_cs);
        }
        else
        {
            m_aa = on;
        }
    }

    void GlyphSource::SetSizes(int latinPx, int cjkPx, unsigned int cjkFrom)
    {
        if (latinPx > 0) m_sizeLatin = latinPx;
        if (cjkPx > 0)   m_sizeCJK   = cjkPx;
        m_cjkFrom = cjkFrom;

        if (!m_faceA)
            return;

        // keep one face per distinct size so nothing thrashes per glyph
        if (m_sizeCJK == m_sizeLatin)
        {
            if (m_faceB)
            {
                CloseFace(&m_faceB);
                if (FT_Set_Pixel_Sizes((FT_Face)m_faceA, 0, (FT_UInt)(m_sizeLatin * m_ss)))
                    return;
            }
        }
        else
        {
            if (!m_faceB)
            {
                m_faceB = OpenFace(m_sizeCJK * m_ss);
            }
            else if (FT_Set_Pixel_Sizes((FT_Face)m_faceB, 0, (FT_UInt)(m_sizeCJK * m_ss)))
            {
                return;
            }
        }

        if (m_csInit)
        {
            EnterCriticalSection(&m_cs);
            m_cache.clear();
            LeaveCriticalSection(&m_cs);
        }
    }

    // Condense the drawn ink horizontally so it fits `advance` columns.
    // Area/OR merging: 1bpp safe, keeps one-pixel-wide strokes, and preserves
    // the vertical metrics (which is what the baseline depends on).
    static void FitColumns(GlyphCell& cell, int advance, int strideBytes, int lines,
                           int fitMode, int baseline)
    {
        if (advance <= 0)
            return;

        int minX = 1 << 30, maxX = -1;
        for (int y = 0; y < lines; ++y)
            for (int x = 0; x < strideBytes * 8; ++x)
                if (cell.bits[y * strideBytes + (x >> 3)] & (0x80 >> (x & 7)))
                {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                }

        if (maxX < 0 || maxX < advance)          // nothing to do
            return;

        const int inkW = maxX - minX + 1;
        const int dstW = advance;                // the first `advance` columns are ours
        unsigned char tmp[3 * 32];
        unsigned char tmpCov[24 * 32];
        memset(tmp, 0, sizeof(tmp));
        memset(tmpCov, 0, sizeof(tmpCov));

        if (fitMode == 0)
        {
            // --- condense: merge source columns into the available cells -----
            // horizontally only, which distorts the aspect ratio (a 9 px Latin
            // cap squeezed into a 7 px cell, CJK squeezed by 10-25%)
            for (int y = 0; y < lines; ++y)
            {
                for (int t = 0; t < dstW; ++t)
                {
                    const int sBegin = minX + (t * inkW) / dstW;
                    const int sEnd   = minX + (((t + 1) * inkW) / dstW) - 1;
                    for (int s = sBegin; s <= sEnd && s <= maxX; ++s)
                    {
                        if (s < minX)
                            continue;
                        if (cell.cov[y * 24 + s] > tmpCov[y * 24 + t])
                            tmpCov[y * 24 + t] = cell.cov[y * 24 + s];
                        if (!(cell.bits[y * strideBytes + (s >> 3)] & (0x80 >> (s & 7))))
                            continue;
                        tmp[y * strideBytes + (t >> 3)] |= (unsigned char)(0x80 >> (t & 7));
                    }
                }
            }
        }
        else
        {
            // --- scale: shrink BOTH axes by the same factor ------------------
            // The glyph keeps its proportions; it only gets a little smaller.
            // Rows shrink towards the baseline and columns towards the pen
            // origin, so placement and line height stay exactly as before.
            int minY = 1 << 30, maxY = -1;
            for (int y = 0; y < lines; ++y)
                for (int x = minX; x <= maxX; ++x)
                    if (cell.bits[y * strideBytes + (x >> 3)] & (0x80 >> (x & 7)))
                    {
                        if (y < minY) minY = y;
                        if (y > maxY) maxY = y;
                    }
            if (maxY < 0)
            {
                memcpy(cell.bits, tmp, sizeof(cell.bits));
                memcpy(cell.cov, tmpCov, sizeof(cell.cov));
                return;
            }

            const int inkH = maxY - minY + 1;
            const int dstH = (inkH * dstW + inkW - 1) / inkW;    // same factor, rounded up
            const int base = baseline;

            for (int ty = 0; ty < dstH; ++ty)
            {
                // destination row, kept on the same side of the baseline
                int dy = (minY >= base) ? base + ((ty * inkH) / dstH) + (minY - base)
                                        : base - (((dstH - ty) * (base - minY) + dstH - 1) / dstH);
                if (dy < 0 || dy >= lines)
                    continue;
                const int yBegin = minY + (ty * inkH) / dstH;
                const int yEnd   = minY + (((ty + 1) * inkH) / dstH) - 1;
                for (int t = 0; t < dstW; ++t)
                {
                    const int xBegin = minX + (t * inkW) / dstW;
                    const int xEnd   = minX + (((t + 1) * inkW) / dstW) - 1;
                    for (int s = xBegin; s <= xEnd && s <= maxX; ++s)
                    {
                        if (s < minX)
                            continue;
                        for (int y = yBegin; y <= yEnd && y <= maxY; ++y)
                        {
                            if (y < minY)
                                continue;
                            if (cell.cov[y * 24 + s] > tmpCov[dy * 24 + t])
                                tmpCov[dy * 24 + t] = cell.cov[y * 24 + s];
                            if (!(cell.bits[y * strideBytes + (s >> 3)] & (0x80 >> (s & 7))))
                                continue;
                            tmp[dy * strideBytes + (t >> 3)] |= (unsigned char)(0x80 >> (t & 7));
                        }
                    }
                }
            }
        }
        memcpy(cell.bits, tmp, sizeof(cell.bits));
        memcpy(cell.cov, tmpCov, sizeof(cell.cov));
    }

    const GlyphCell* GlyphSource::Get(unsigned int codepoint, int gameAdvance, int phase)
    {
        if (!m_faceA)
            return NULL;

        // phase is a SIGNED shift in quarter pixels (-3..3).  The outline is
        // rasterised at size*m_ss, so a shift of one screen pixel is m_ss ppem
        // there: in 26.6 units that is m_ss * 64, hence phase * 16 * m_ss.
        if (phase < -3) phase = -3;
        if (phase >  3) phase =  3;

        // cache key: codepoint + the ACTUAL advance used + subpixel phase.
        // It used to store only "an advance was supplied", so once the published
        // advance changed (Metrics=scaled writes it back) the cache still served
        // the cell rasterised for the old advance - measuring, caching and
        // drawing then disagreed (overlapping Latin, cramped punctuation).
        const int advKey = (gameAdvance > 0) ? (gameAdvance > 255 ? 255 : gameAdvance) : 0;
        const unsigned int key = codepoint | ((unsigned int)advKey << 23)
                                          | ((unsigned int)((phase + 3) & 7) << 31);

        EnterCriticalSection(&m_cs);
        std::map<unsigned int, GlyphCell>::iterator it = m_cache.find(key);
        if (it != m_cache.end())
        {
            const GlyphCell* hit = &it->second;   // map nodes are stable
            LeaveCriticalSection(&m_cs);
            return hit;
        }
        LeaveCriticalSection(&m_cs);

        // ---- rasterise (serialised: one FT_Face is not thread safe) ---------
        // A miss happens once per glyph, so holding the lock here costs nothing
        // in practice while making concurrent calls safe.
        EnterCriticalSection(&m_cs);
        {
            std::map<unsigned int, GlyphCell>::iterator again = m_cache.find(key);
            if (again != m_cache.end())
            {
                const GlyphCell* hit = &again->second;
                LeaveCriticalSection(&m_cs);
                return hit;
            }

            // bounded cache: a pathological stream of distinct codepoints must
            // not grow without limit (131k keys x ~900 B would be ~118 MB)
            if (m_cache.size() >= 16384)
                m_cache.clear();

            FT_Face face = (FT_Face)((codepoint >= m_cjkFrom && m_faceB) ? m_faceB : m_faceA);

            // bake the subpixel phase in: a quarter-pixel horizontal delta makes
            // FreeType shift the coverage, and TARGET_LIGHT keeps horizontal
            // hinting from snapping it back onto the pixel grid
            FT_Matrix mat;
            FT_Vector delta;
            mat.xx = 1 << 16; mat.xy = 0;
            mat.yx = 0;       mat.yy = 1 << 16;
            delta.x = (FT_Pos)(phase * 16 * m_ss);   // quarter pixels -> rasteriser space
            delta.y = 0;
            FT_Set_Transform(face, &mat, phase ? &delta : NULL);

            const bool aa = m_aa;
            // Subpixel phases keep LIGHT hinting: vertical hinting is exactly what
            // preserves one-pixel horizontal strokes at these sizes.  Fully
            // unhinted outlines (FT_LOAD_NO_HINTING) lost them, which is why
            // horizontal strokes turned into dotted lines.
            const FT_Int32 loadFlags = (aa ? FT_LOAD_TARGET_LIGHT : FT_LOAD_TARGET_MONO) | FT_LOAD_RENDER;
            if (FT_Load_Char(face, (FT_ULong)codepoint, loadFlags))
            {
                LeaveCriticalSection(&m_cs);
                return NULL;
            }

            const FT_GlyphSlot g = face->glyph;

            GlyphCell cell;
            memset(&cell, 0, sizeof(cell));

            const int advance = (gameAdvance > 0) ? gameAdvance : (int)(g->advance.x >> 6);
            cell.width = (unsigned char)(advance < 0 ? 0 : (advance > 255 ? 255 : advance));

            // Rasterise straight into the cell when the factor is 1, otherwise
            // accumulate in supersampled space and box-filter down.  Placement
            // happens in supersampled space, so the baseline stays exact for any
            // factor (no bearing rounding).
            const int ss = m_ss;
            if (ss == 1)
            {
                const int x0 = g->bitmap_left;
                const int y0 = m_baseline - g->bitmap_top;
                for (unsigned int r = 0; r < g->bitmap.rows; ++r)
                {
                    const int y = y0 + (int)r;
                    if (y < 0 || y >= m_lines)
                        continue;
                    const unsigned char* src = g->bitmap.buffer + r * g->bitmap.pitch;
                    for (unsigned int c = 0; c < g->bitmap.width; ++c)
                    {
                        const int x = x0 + (int)c;
                        if (x < 0 || x >= m_stride * 8)
                            continue;
                        const int cov = (g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                            ? (((src[c >> 3] >> (7 - (c & 7))) & 1) ? 255 : 0)
                            : src[c];
                        if (!cov)
                            continue;
                        cell.cov[y * 24 + x] = (unsigned char)cov;
                        if (cov >= 128)
                            cell.bits[y * m_stride + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
                    }
                }
            }
            else
            {
                static unsigned short acc[24 * 32];
                memset(acc, 0, sizeof(acc));

                const int w = m_stride * 8;

                // Anti-clip: a vector ideograph reaches further above the baseline
                // than the game's bitmap cell, so its top rows used to land above
                // row 0 and were dropped - the visible "top of the glyph is cut"
                // defect.  If the ink top would overflow, shift the whole glyph
                // down by whole cell rows so the top is preserved.
                const int ssTop = m_baseline * ss - g->bitmap_top;
                int shiftRows = 0;
                if (ssTop < 0)
                    shiftRows = ((-ssTop) + ss - 1) / ss;

                for (unsigned int r = 0; r < g->bitmap.rows; ++r)
                {
                    const int sy = ssTop + shiftRows * ss + (int)r;
                    if (sy < 0 || sy >= m_lines * ss)
                        continue;
                    const unsigned char* src = g->bitmap.buffer + r * g->bitmap.pitch;
                    for (unsigned int c = 0; c < g->bitmap.width; ++c)
                    {
                        const int sx = g->bitmap_left + (int)c;
                        if (sx < 0 || sx >= w * ss)
                            continue;
                        const int cov = (g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                            ? (((src[c >> 3] >> (7 - (c & 7))) & 1) ? 255 : 0)
                            : src[c];
                        if (!cov)
                            continue;
                        unsigned short* a = acc + (sy / ss) * w + (sx / ss);
                        *a = (unsigned short)((*a + cov > 65535) ? 65535 : (*a + cov));
                    }
                }

                const int div = ss * ss;

                for (int y = 0; y < m_lines; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        int cov = acc[y * w + x] / div;
                        if (cov > 255)
                            cov = 255;
                        if (!cov)
                            continue;
                        cell.cov[y * 24 + x] = (unsigned char)cov;
                        if (cov >= 128)
                            cell.bits[y * m_stride + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
                    }
            }

            // Align the ink to the same row the original 16x16 bitmap uses for
            // this class of character.  Measured from game.fnt itself:
            //   CJK ideographs / fullwidth : ink top row 0
            //   ASCII lower case           : ink top row 3
            //   ASCII upper / digits / punct: ink top row 4
            if (m_classAlign)
            {
                int top = -1, bot = -1;
                for (int y = 0; y < m_lines && top < 0; ++y)
                    for (int x = 0; x < m_stride * 8; ++x)
                        if (cell.bits[y * m_stride + (x >> 3)] & (0x80 >> (x & 7)))
                        {
                            top = y;
                            break;
                        }
                if (top >= 0)
                {
                    for (int y = m_lines - 1; y >= 0 && bot < 0; --y)
                        for (int x = 0; x < m_stride * 8; ++x)
                            if (cell.bits[y * m_stride + (x >> 3)] & (0x80 >> (x & 7)))
                            {
                                bot = y;
                                break;
                            }

                    // game.fnt is a 16x16 bitmap face: ideographs fill all 16
                    // rows, bottom-hugging punctuation sits in 12..15, lower
                    // case ascenders start at 3 and x-height letters at 6.
                    const bool cjkPunct = (codepoint >= 0x3001 && codepoint <= 0x3011) ||
                                          codepoint == 0xFF0C || codepoint == 0xFF1A ||
                                          codepoint == 0xFF1B || codepoint == 0xFF01 ||
                                          codepoint == 0xFF1F || codepoint == '.' ||
                                          codepoint == ',';
                    const bool ascender = codepoint == 'b' || codepoint == 'd' || codepoint == 'f' ||
                                          codepoint == 'h' || codepoint == 'k' || codepoint == 'l' ||
                                          codepoint == 't';
                    const int target = cjkPunct ? -1                        // bottom aligned
                                     : (codepoint >= 0x2E80) ? 0
                                     : (codepoint >= 'a' && codepoint <= 'z') ? (ascender ? 3 : 6) : 4;
                    int dy = (target < 0) ? ((m_lines - 1) - bot) : (target - top);
                    if (dy > 3) dy = 3;
                    if (dy < -3) dy = -3;
                    if (bot + dy > m_lines - 1) dy = (m_lines - 1) - bot;
                    if (top + dy < 0) dy = -top;

                    // Ideographs fill the whole 16x16 design box in game.fnt,
                    // while a vector ideograph is shorter - stretch the ink rows
                    // over 0..15 so the text occupies the same band.
                    const bool ideograph = (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
                                           (codepoint >= 0xFF01 && codepoint <= 0xFF5E);
                    if (ideograph && bot > top && (bot - top + 1) < m_lines)
                    {
                        unsigned char nb[3 * 32];
                        unsigned char nc[24 * 32];
                        memset(nb, 0, sizeof(nb));
                        memset(nc, 0, sizeof(nc));
                        for (int y = top; y <= bot; ++y)
                        {
                            int ny = ((y - top) * (m_lines - 1)) / (bot - top);
                            if (ny < 0) ny = 0;
                            if (ny > m_lines - 1) ny = m_lines - 1;
                            for (int x = 0; x < m_stride * 8; ++x)
                            {
                                if (cell.cov[y * 24 + x] > nc[ny * 24 + x])
                                    nc[ny * 24 + x] = cell.cov[y * 24 + x];
                                if (cell.bits[y * m_stride + (x >> 3)] & (0x80 >> (x & 7)))
                                    nb[ny * m_stride + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
                            }
                        }
                        memcpy(cell.bits, nb, sizeof(cell.bits));
                        memcpy(cell.cov, nc, sizeof(cell.cov));
                        dy = 0;
                    }

                    if (dy > 0)
                    {
                        for (int y = m_lines - 1; y >= dy; --y)
                        {
                            memcpy(cell.bits + y * m_stride, cell.bits + (y - dy) * m_stride, m_stride);
                            memcpy(cell.cov + y * 24, cell.cov + (y - dy) * 24, 24);
                        }
                        memset(cell.bits, 0, (size_t)dy * m_stride);
                        memset(cell.cov, 0, (size_t)dy * 24);
                    }
                    else if (dy < 0)
                    {
                        for (int y = 0; y < m_lines + dy; ++y)
                        {
                            memcpy(cell.bits + y * m_stride, cell.bits + (y - dy) * m_stride, m_stride);
                            memcpy(cell.cov + y * 24, cell.cov + (y - dy) * 24, 24);
                        }
                        memset(cell.bits + (m_lines + dy) * m_stride, 0, (size_t)(-dy) * m_stride);
                        memset(cell.cov + (m_lines + dy) * 24, 0, (size_t)(-dy) * 24);
                    }
                }
            }

            if (m_fit)
                FitColumns(cell, advance, m_stride, m_lines, m_fitMode, m_baseline);

            std::pair<std::map<unsigned int, GlyphCell>::iterator, bool> ins =
                m_cache.insert(std::make_pair(key, cell));
            const GlyphCell* result = &ins.first->second;
            LeaveCriticalSection(&m_cs);
            return result;
        }
    }
}
