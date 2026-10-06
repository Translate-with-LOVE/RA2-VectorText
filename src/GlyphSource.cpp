#include "GlyphSource.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_MODULE_H
#include FT_OUTLINE_H

#include <string.h>
#include <stdlib.h>

namespace vt
{
    GlyphSource::GlyphSource()
        : m_lib(NULL), m_faceA(NULL), m_faceB(NULL), m_size(13),
          m_sizeLatin(13), m_sizeCJK(16), m_cjkFrom(0x2E80u),
          m_stride(3), m_lines(16), m_baseline(13), m_fit(true), m_aa(false), m_darkening(0), m_ss(1), m_fitMode(1), m_darkErr{0,0,0}, m_classAlign(true),
          m_path(NULL), m_weight(400), m_csInit(false)
    {
        m_latinPath[0] = 0;
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

    void* GlyphSource::OpenFace(int pixelSize, const char* path)
    {
        if (!m_lib || !m_path)
            return NULL;

        FT_Face face = NULL;
        if (FT_New_Face((FT_Library)m_lib, path && *path ? path : m_path, 0, &face))
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
        m_latinPath[0] = 0;
        m_weight   = weight;

        FT_Library lib = NULL;
        if (FT_Init_FreeType(&lib))
            return false;
        m_lib = lib;

        // Auto-hinter properties are Boolean/module-specific, not a numeric
        // "darkening" property on the TrueType driver. Record unsupported knobs.
        memset(m_darkErr, 0, sizeof(m_darkErr));
        if (m_darkening > 0)
        {
            FT_Bool noDark = 0;
            m_darkErr[0] = FT_Property_Set(lib, "autofitter", "no-stem-darkening", &noDark);
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
        if (m_sizeCJK != m_sizeLatin || m_latinPath[0])
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

        if (FT_Set_Pixel_Sizes((FT_Face)m_faceA, 0, (FT_UInt)(m_sizeLatin * m_ss)))
            return;
        if (m_sizeCJK == m_sizeLatin && !m_latinPath[0])
            CloseFace(&m_faceB);
        else if (!m_faceB)
            m_faceB = OpenFace(m_sizeCJK * m_ss);
        else if (FT_Set_Pixel_Sizes((FT_Face)m_faceB, 0, (FT_UInt)(m_sizeCJK * m_ss)))
            return;

        if (m_csInit)
        {
            EnterCriticalSection(&m_cs);
            m_cache.clear();
            LeaveCriticalSection(&m_cs);
        }
    }

    bool GlyphSource::SetLatinFont(const char* path)
    {
        if (!m_faceA || !path || !*path)
            return false;
        void* latin = OpenFace(m_sizeLatin * m_ss, path);
        if (!latin)
            return false;
        if (!m_faceB)
            m_faceB = OpenFace(m_sizeCJK * m_ss);
        if (!m_faceB)
        {
            CloseFace(&latin);
            return false;
        }
        EnterCriticalSection(&m_cs);
        CloseFace(&m_faceA);
        m_faceA = latin;
        strncpy_s(m_latinPath, path, _TRUNCATE);
        m_cache.clear();
        LeaveCriticalSection(&m_cs);
        return true;
    }

    bool GlyphSource::UsesCJKFace(unsigned int cp) const
    {
        // These characters lie below U+2E80 but belong with Chinese typography
        // in this game's text. ASCII punctuation stays with the Latin face.
        return cp >= m_cjkFrom || cp == 0x2013 || cp == 0x2014 || cp == 0x2015 ||
               (cp >= 0x2018 && cp <= 0x201F) || cp == 0x2025 || cp == 0x2026;
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
        const unsigned long long key = (unsigned long long)codepoint
            | ((unsigned long long)advKey << 32)
            | ((unsigned long long)(phase + 3) << 40);

        EnterCriticalSection(&m_cs);
        std::map<unsigned long long, GlyphCell>::iterator it = m_cache.find(key);
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
            std::map<unsigned long long, GlyphCell>::iterator again = m_cache.find(key);
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

            FT_Face face = (FT_Face)FaceHandleFor(codepoint);

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
            const FT_Int32 loadFlags = (aa ? FT_LOAD_TARGET_LIGHT : FT_LOAD_TARGET_MONO) | FT_LOAD_NO_BITMAP;
            if (FT_Load_Char(face, (FT_ULong)codepoint, loadFlags))
            {
                LeaveCriticalSection(&m_cs);
                return NULL;
            }

            const FT_GlyphSlot g = face->glyph;

            GlyphCell cell;
            memset(&cell, 0, sizeof(cell));

            const int advance = (gameAdvance > 0) ? gameAdvance : (int)((g->advance.x + 32 * m_ss) / (64 * m_ss));
            cell.width = (unsigned char)(advance < 0 ? 0 : (advance > 255 ? 255 : advance));

            // Fit the OUTLINE uniformly before rendering. Never resize an
            // already rasterised bitmap or stretch ideographs to fill 16 rows.
            // Keep the same baseline for all letters, including descenders.
            const int ss = m_ss;
            if (g->format == FT_GLYPH_FORMAT_OUTLINE && g->outline.n_points)
            {
                FT_BBox box;
                FT_Outline_Get_CBox(&g->outline, &box);
                const double unit = 64.0 * ss;
                const double inkW = (box.xMax - box.xMin) / unit;
                const double inkH = (box.yMax - box.yMin) / unit;
                const bool smallMark = codepoint == '.' || codepoint == ',' ||
                    codepoint == ':' || codepoint == ';' || codepoint == '!' || codepoint == '?' ||
                    codepoint == 0x3001 || codepoint == 0x3002 || codepoint == 0xFF0C ||
                    codepoint == 0xFF0E || codepoint == 0xFF1A || codepoint == 0xFF1B ||
                    codepoint == 0xFF01 || codepoint == 0xFF1F;
                const bool opening = codepoint == '(' || codepoint == '[' || codepoint == '{' ||
                    codepoint == 0xFF08 || codepoint == 0x3010 || codepoint == 0x300C ||
                    codepoint == 0x300E || codepoint == 0x2018 || codepoint == 0x201C;
                const bool closing = codepoint == ')' || codepoint == ']' || codepoint == '}' ||
                    codepoint == 0xFF09 || codepoint == 0x3011 || codepoint == 0x300D ||
                    codepoint == 0x300F || codepoint == 0x2019 || codepoint == 0x201D;
                const bool dash = codepoint == 0x2013 || codepoint == 0x2014 || codepoint == 0x2015;
                const bool symbol = smallMark || opening || closing || dash ||
                    codepoint == 0x2025 || codepoint == 0x2026;
                const bool fitCell = m_fit && gameAdvance > 0;
                // Narrow legacy punctuation cells need fractional breathing room.
                // Reserve only 1/4 pixel per side; outline scaling remains uniform.
                const double inset = fitCell && symbol && gameAdvance >= 2 ? 0.25 : 0.0;
                const double limit = fitCell ? (double)gameAdvance : 24.0;
                const double available = limit - 2.0 * inset;
                double scale = 1.0;
                if (fitCell && inkW > available)
                    scale = available / inkW;
                if (inkH > m_lines && scale > m_lines / inkH)
                    scale = m_lines / inkH;
                mat.xx = mat.yy = (FT_Fixed)(scale * 65536.0 + 0.5);
                // The CBox already includes the initial phase. Reposition
                // bearings that overflow the cell without changing the shape.
                const double initialPhase = phase * 0.25;
                double left = (box.xMin / unit - initialPhase) * scale + initialPhase;
                double right = (box.xMax / unit - initialPhase) * scale + initialPhase;
                double offset = 0.0;
                if (fitCell && symbol)
                {
                    // Align paired marks toward the text they enclose; centre
                    // stops/colons in their existing cell. The pen never changes.
                    double targetLeft = (limit - inkW * scale) * 0.5;
                    if (opening) targetLeft = limit - inset - inkW * scale;
                    if (closing) targetLeft = inset;
                    offset = targetLeft - left;
                }
                if (right + offset > limit - inset) offset = limit - inset - right;
                if (left + offset < inset) offset = inset - left;
                delta.x = (FT_Pos)((initialPhase + offset) * unit);
                // Translate only if the full outline would be vertically clipped.
                const double top = m_baseline - box.yMax / unit * scale;
                const double bottom = m_baseline - box.yMin / unit * scale;
                double shiftY = top < 0.0 ? -top : 0.0;
                if (bottom + shiftY > m_lines) shiftY = m_lines - bottom;
                delta.y = (FT_Pos)(-shiftY * unit);
                FT_Set_Transform(face, &mat, &delta);
            }
            if (FT_Load_Char(face, (FT_ULong)codepoint, loadFlags | FT_LOAD_RENDER))
            {
                LeaveCriticalSection(&m_cs);
                return NULL;
            }

            // Area coverage from the high-resolution bitmap. Every source
            // sample contributes to its target pixel, so no rows are skipped.
            unsigned int acc[24 * 32] = { 0 };
            for (unsigned int r = 0; r < g->bitmap.rows; ++r)
            {
                const int sy = m_baseline * ss - g->bitmap_top + (int)r;
                if (sy < 0 || sy >= m_lines * ss)
                    continue;
                const unsigned char* src = g->bitmap.buffer + r * g->bitmap.pitch;
                for (unsigned int c = 0; c < g->bitmap.width; ++c)
                {
                    const int sx = g->bitmap_left + (int)c;
                    if (sx < 0 || sx >= m_stride * 8 * ss)
                        continue;
                    const int cov = (g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                        ? (((src[c >> 3] >> (7 - (c & 7))) & 1) ? 255 : 0)
                        : src[c];
                    acc[(sy / ss) * 24 + sx / ss] += cov;
                }
            }
            const unsigned int div = ss * ss;
            for (int y = 0; y < m_lines; ++y)
                for (int x = 0; x < m_stride * 8; ++x)
                {
                    int cov = (acc[y * 24 + x] + div / 2) / div;
                    cell.cov[y * 24 + x] = (unsigned char)cov;
                    if (cov >= 128)
                        cell.bits[y * m_stride + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
                }

            std::pair<std::map<unsigned long long, GlyphCell>::iterator, bool> ins =
                m_cache.insert(std::make_pair(key, cell));
            const GlyphCell* result = &ins.first->second;
            LeaveCriticalSection(&m_cs);
            return result;
        }
    }
}
