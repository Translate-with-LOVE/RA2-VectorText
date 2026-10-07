// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
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
        : m_lib(NULL), m_faceA(NULL), m_faceB(NULL),
          m_sizeLatin(13), m_sizeCJK(16), m_cjkFrom(0x2E80u),
          m_stride(3), m_lines(16), m_baseline(13), m_fit(true), m_aa(false), m_darkening(0), m_ss(1), m_hinting(0), m_darkErr{0,0,0},
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

    void GlyphSource::ClearCache()
    {
        m_cache.clear(); m_highCache.clear();
        CloseFace(&m_highA); CloseFace(&m_highB);
    }

    void GlyphSource::SetHighResolution(bool on)
    {
        EnterCriticalSection(&m_cs);
        if(m_highResolution!=on) { ClearCache(); m_highResolution=on; }
        LeaveCriticalSection(&m_cs);
    }

    void* GlyphSource::OpenFace(int pixelSize, const char* path)
    {
        if (!m_lib || !m_path)
            return NULL;

        FT_Face face = NULL;
        if (FT_New_Face((FT_Library)m_lib, path && *path ? path : m_path, 0, &face))
            return NULL;

        // Apply the configured wght coordinate when the variable face has that
        // axis; leave other axes at their defaults. Static faces have no axes.
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
            ClearCache();
            LeaveCriticalSection(&m_cs);
        }
        return true;
    }

    void GlyphSource::Shutdown()
    {
        CloseFace(&m_highB);
        CloseFace(&m_highA);
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
            ClearCache();
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
            ClearCache();
            LeaveCriticalSection(&m_cs);
        }
    }

    void GlyphSource::SetAntiAlias(bool on)
    {
        if (m_csInit)
        {
            EnterCriticalSection(&m_cs);
            m_aa = on;
            ClearCache();                 // coverage is baked into the cell
            LeaveCriticalSection(&m_cs);
        }
        else
        {
            m_aa = on;
        }
    }

    void GlyphSource::SetHinting(int mode)
    {
        if (mode < 0 || mode > 2) mode = 0;
        EnterCriticalSection(&m_cs);
        if (mode != m_hinting)
        {
            m_hinting = mode;
            ClearCache();
        }
        LeaveCriticalSection(&m_cs);
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
            ClearCache();
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
        ClearCache();
        LeaveCriticalSection(&m_cs);
        return true;
    }

    void GlyphSource::SetLegacyCodepage1252(bool on)
    {
        EnterCriticalSection(&m_cs);
        if (m_legacy1252 != on) { m_legacy1252 = on; ClearCache(); }
        LeaveCriticalSection(&m_cs);
    }

    unsigned int GlyphSource::RenderCodepoint(unsigned int cp) const
    {
        // Undefined CP1252 positions retain their original values. Only this
        // bounded C1 range is reinterpreted; Unicode and ASCII stay unchanged.
        static const unsigned short unicode[32] = {
            0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,
            0x02C6,0x2030,0x0160,0x2039,0x0152,0x008D,0x017D,0x008F,
            0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
            0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x0178
        };
        return m_legacy1252 && cp >= 0x80 && cp <= 0x9F ? unicode[cp-0x80] : cp;
    }

    bool GlyphSource::UsesCJKFace(unsigned int cp) const
    {
        // These characters lie below U+2E80 but belong with Chinese typography
        // in this game's text. ASCII punctuation stays with the Latin face.
        return cp >= m_cjkFrom || cp == 0x2013 || cp == 0x2014 || cp == 0x2015 ||
               (cp >= 0x2018 && cp <= 0x201F) || cp == 0x2025 || cp == 0x2026;
    }

    const GlyphCell* GlyphSource::Get(unsigned int codepoint, int gameAdvance, int phase, int scale1024)
    {
        codepoint = RenderCodepoint(codepoint);
        if (!m_faceA)
            return NULL;
        if (scale1024 < 1 || scale1024 > 1024) return NULL;

        // Signed phase is in quarter logical pixels (-3..3). One logical pixel
        // spans m_ss raster pixels: in FreeType 26.6 units, the shift is
        // phase * 16 * m_ss.
        if (phase < -3) phase = -3;
        if (phase >  3) phase =  3;

        // Cache key includes codepoint, clamped compatibility advance (0 means
        // natural), signed raster phase and uniform row scale. Each variant
        // needs its own coverage; font/raster setting changes clear the cache.
        const int advKey = (gameAdvance > 0) ? (gameAdvance > 255 ? 255 : gameAdvance) : 0;
        const unsigned long long key = (unsigned long long)codepoint
            | ((unsigned long long)advKey << 32)
            | ((unsigned long long)(phase + 3) << 40)
            | ((unsigned long long)scale1024 << 44);

        EnterCriticalSection(&m_cs);
        std::map<unsigned long long, GlyphCell>::iterator it = m_cache.find(key);
        if (it != m_cache.end())
        {
            const GlyphCell* hit = &it->second;   // stable until ClearCache/Shutdown; callers must not retain across those
            LeaveCriticalSection(&m_cs);
            return hit;
        }
        LeaveCriticalSection(&m_cs);

        // ---- rasterise (serialised: one FT_Face is not thread safe) ---------
        // A miss is per codepoint/advance/phase/scale variant. Recheck after
        // locking in case another caller populated it; hits avoid raster work.
        EnterCriticalSection(&m_cs);
        {
            std::map<unsigned long long, GlyphCell>::iterator again = m_cache.find(key);
            if (again != m_cache.end())
            {
                const GlyphCell* hit = &again->second;
                LeaveCriticalSection(&m_cs);
                return hit;
            }

            // Bound logical variants to 16384 entries. Clear their optional 2x
            // rasters together; raster allocations make memory use variable.
            if (m_cache.size() >= 16384)
                ClearCache();

            FT_Face face = (FT_Face)FaceHandleFor(codepoint);
            if (!FT_Get_Char_Index(face, codepoint))
            {
                LeaveCriticalSection(&m_cs);
                return NULL;
            }

            // The transform runs after grid fitting: fractional pen positions
            // shift the antialiasing coverage without moving the logical pen.
            FT_Matrix mat;
            FT_Vector delta;
            mat.xx = scale1024 * 64; mat.xy = 0;
            mat.yx = 0;              mat.yy = scale1024 * 64;
            delta.x = (FT_Pos)(phase * 16 * m_ss);   // quarter pixels -> rasteriser space
            delta.y = 0;
            FT_Set_Transform(face, &mat, phase ? &delta : NULL);

            const bool aa = m_aa;
            // LIGHT preserves spacing while aligning horizontal strokes to the
            // vertical grid. NORMAL and unhinted are available for comparison.
            const FT_Int32 grayFlags = m_hinting == 1 ? FT_LOAD_TARGET_NORMAL :
                (m_hinting == 2 ? FT_LOAD_NO_HINTING : FT_LOAD_TARGET_LIGHT);
            const FT_Int32 loadFlags = (aa ? grayFlags : FT_LOAD_TARGET_MONO) | FT_LOAD_NO_BITMAP;
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
            // Natural layout follows the font's fractional design advance,
            // independent of raster hinting and sampling resolution. Otherwise
            // normal hinting at 1x rounds every letter's pen to whole pixels.
            cell.advanceQ = (int)floor((double)g->linearHoriAdvance * scale1024 /
                (16384.0 * m_ss * 1024.0) + 0.5);

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
                mat.xx = mat.yy = (FT_Fixed)(scale * scale1024 * 64.0 + 0.5);
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
                if (gameAdvance > 0)
                {
                    if (right + offset > limit - inset) offset = limit - inset - right;
                    if (left + offset < inset) offset = inset - left;
                }
                cell.inkLeftQ = (int)floor((left + offset) * 4.0 + 0.5);
                cell.inkRightQ = (int)ceil((right + offset) * 4.0);
                delta.x = (FT_Pos)((initialPhase + offset) * unit);
                // All direct-drawing glyphs share the same baseline. Ink above
                // or below the old 16-row cell must retain its actual bearing;
                // fitting each ink box vertically made Chinese letters bounce.
                delta.y = 0;
                FT_Set_Transform(face, &mat, &delta);
            }
            if (FT_Load_Char(face, (FT_ULong)codepoint, loadFlags | FT_LOAD_RENDER))
            {
                LeaveCriticalSection(&m_cs);
                return NULL;
            }

            // Area coverage from the high-resolution bitmap. Every source
            // sample contributes to its target pixel, so no rows are skipped.
            int rows = m_lines;
            if (gameAdvance <= 0)
                cell.inkX = (int)floor((double)g->bitmap_left / ss);
            if (g->bitmap.rows)
            {
                const int firstY = m_baseline * ss - g->bitmap_top;
                cell.inkY = (int)floor((double)firstY / ss);
                rows = (firstY + (int)g->bitmap.rows - cell.inkY * ss + ss - 1) / ss;
                if (rows > 32)
                {
                    LeaveCriticalSection(&m_cs);
                    return NULL;
                }
                cell.inkRows = rows;
            }
            if (gameAdvance <= 0 && (int)g->bitmap.width + g->bitmap_left - cell.inkX * ss > 24 * ss)
            {
                LeaveCriticalSection(&m_cs);
                return NULL;
            }
            unsigned int acc[24 * 32] = { 0 };
            for (unsigned int r = 0; r < g->bitmap.rows; ++r)
            {
                const int sy = m_baseline * ss - g->bitmap_top + (int)r - cell.inkY * ss;
                if (sy < 0 || sy >= rows * ss)
                    continue;
                const unsigned char* src = g->bitmap.buffer + r * g->bitmap.pitch;
                for (unsigned int c = 0; c < g->bitmap.width; ++c)
                {
                    const int sx = g->bitmap_left + (int)c - cell.inkX * ss;
                    if (sx < 0 || sx >= m_stride * 8 * ss)
                        continue;
                    const int cov = (g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                        ? (((src[c >> 3] >> (7 - (c & 7))) & 1) ? 255 : 0)
                        : src[c];
                    acc[(sy / ss) * 24 + sx / ss] += cov;
                }
            }
            const unsigned int div = ss * ss;
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < m_stride * 8; ++x)
                {
                    int cov = (acc[y * 24 + x] + div / 2) / div;
                    // Fixed-point outline fitting can leave a tiny fringe just
                    // beyond an explicit legacy cell. Clip that compatibility
                    // path exactly; natural line glyphs keep their bearings.
                    if (m_fit && gameAdvance > 0 && x >= gameAdvance) cov = 0;
                    cell.cov[y * 24 + x] = (unsigned char)cov;
                    if (cov >= 128)
                        cell.bits[y * m_stride + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
                }

            // Retain a separate raster with hinting on a fixed 2x grid. D3D9
            // scales these samples to the actual viewport, including fractions;
            // logical advances/bearings above remain the same 1x layout.
            if(m_highResolution)
            {
                const bool cjk=UsesCJKFace(codepoint) && m_faceB;
                void** high=cjk ? &m_highB : &m_highA;
                if(!*high) *high=OpenFace((cjk ? m_sizeCJK : m_sizeLatin)*2,
                    cjk ? NULL : (m_latinPath[0] ? m_latinPath : NULL));
                FT_Face hf=(FT_Face)*high;
                FT_Vector hd={delta.x*2/m_ss,delta.y*2/m_ss};
                if(hf) FT_Set_Transform(hf,&mat,&hd);
                if(hf && !FT_Load_Char(hf,(FT_ULong)codepoint,loadFlags|FT_LOAD_RENDER))
                {
                    const auto& bitmap=hf->glyph->bitmap;
                    if(bitmap.width<=128 && bitmap.rows<=128)
                    {
                        GlyphRaster2 raster;
                        raster.left=hf->glyph->bitmap_left;
                        raster.top=m_baseline*2-hf->glyph->bitmap_top;
                        raster.width=(int)bitmap.width; raster.rows=(int)bitmap.rows;
                        raster.coverage.resize((size_t)raster.width*raster.rows);
                        for(int r=0;r<raster.rows;++r) for(int c=0;c<raster.width;++c)
                        {
                            const auto* src=bitmap.buffer+r*bitmap.pitch;
                            int cov=bitmap.pixel_mode==FT_PIXEL_MODE_MONO ?
                                ((src[c/8]&(0x80>>(c&7))) ? 255:0) : src[c];
                            if(m_fit && gameAdvance>0 && (raster.left+c<0 || raster.left+c>=gameAdvance*2)) cov=0;
                            raster.coverage[(size_t)r*raster.width+c]=(unsigned char)cov;
                        }
                        cell.raster2=&m_highCache.emplace(key,std::move(raster)).first->second;
                    }
                }
            }
            std::pair<std::map<unsigned long long, GlyphCell>::iterator, bool> ins =
                m_cache.insert(std::make_pair(key, cell));
            const GlyphCell* result = &ins.first->second;
            LeaveCriticalSection(&m_cs);
            return result;
        }
    }

    int GlyphSource::KerningQuarter(unsigned int left, unsigned int right)
    {
        left = RenderCodepoint(left); right = RenderCodepoint(right);
        if (!left || !right || !m_faceA) return 0;
        EnterCriticalSection(&m_cs);
        FT_Face face = (FT_Face)FaceHandleFor(left);
        FT_Vector delta = { 0, 0 };
        if (face == FaceHandleFor(right) && FT_HAS_KERNING(face))
            FT_Get_Kerning(face, FT_Get_Char_Index(face, left), FT_Get_Char_Index(face, right),
                           FT_KERNING_UNFITTED, &delta);
        const int q = (int)floor((double)delta.x / (16 * m_ss) + 0.5);
        LeaveCriticalSection(&m_cs);
        return q;
    }
}
