// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Atlas.h"

// Sparse 2x glyph atlas, filter gutters and actual-viewport geometry.
namespace vt::Presentation32::detail
{
bool AtlasShape(size_t count, int maxWidth, int maxHeight, int &width, int &height, int &columns)
{
    width = 1024;
    while (width > maxWidth)
        width /= 2;
    while (width >= AtlasCellW)
    {
        columns = width / AtlasCellW;
        const size_t rows = (std::max<size_t>(1, count) + columns - 1) / columns;
        height = 1;
        while ((size_t)height < rows * AtlasCellH && height <= maxHeight)
            height *= 2;
        if (height <= maxHeight)
            return true;
        if (width > maxWidth / 2)
            return false;
        width *= 2;
    }
    return false;
}
void PackOverlay(const PixelPlane &plane, const std::vector<PixelRect> &tiles, uint32_t *output, int pitch, int columns)
{
    for (size_t i = 0; i < tiles.size(); ++i)
    {
        const int x = (int)(i % columns) * AtlasCellW, y = (int)(i / columns) * AtlasCellH;
        auto *pixels = output + (size_t)y * pitch + x;
        for (int row = 0; row < AtlasCellH; ++row)
            memset(pixels + (size_t)row * pitch, 0, AtlasCellW * 4);
        const auto r = tiles[i];
        plane.Overlay2Rect(pixels, pitch, {r.left - 1, r.top - 1, r.right + 1, r.bottom + 1});
    }
}
bool OutputScale(const Texture &t, const ScreenVertex (&vertices)[4], float &sx, float &sy)
{
    for (const auto &v : vertices)
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.u) || !std::isfinite(v.v))
            return false;
    const float sourceW = (vertices[2].u - vertices[0].u) * t.width;
    const float sourceH = (vertices[0].v - vertices[1].v) * t.height;
    sx = sourceW > 0 ? (vertices[2].x - vertices[0].x) / sourceW : 0;
    sy = sourceH > 0 ? (vertices[0].y - vertices[1].y) / sourceH : 0;
    return std::isfinite(sx) && std::isfinite(sy) && sx > 1.001f && sy > 1.001f &&
           fabs(sx - sy) <= 0.001f * std::max(sx, sy);
}
bool ResizeAtlas(Texture &t, size_t count)
{
    if (t.overlay && count <= (size_t)t.atlasColumns * (t.atlasHeight / AtlasCellH))
        return true;
    D3DCAPS9 caps{};
    if (!t.device || FAILED(t.device->GetDeviceCaps(&caps)))
        return false;
    int columns = 0, height = 0, width = 0;
    if (!AtlasShape(count, caps.MaxTextureWidth, caps.MaxTextureHeight, width, height, columns))
        return false;
    if (t.overlay && columns == t.atlasColumns && height <= t.atlasHeight)
        return true;
    IDirect3DTexture9 *atlas = nullptr;
    auto create = (CreateTexture)Original(t.device, 23);
    if (FAILED(create(t.device, width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, (void **)&atlas, nullptr)))
        return false;
    if (t.overlay)
        t.overlay->Release();
    t.overlay = atlas;
    t.atlasWidth = width;
    t.atlasHeight = height;
    t.atlasColumns = columns;
    Log::Note("Present32: sparse text atlas %dx%d (%.2f MiB)", width, height, width * height / 262144.0);
    return true;
}

bool UploadOverlay(Texture &t, const PixelPlane &plane)
{
    auto tiles = plane.TextTiles(true);
    if (tiles.empty())
    {
        t.overlayTiles.clear();
        return true;
    }
    if (!ResizeAtlas(t, tiles.size()))
        return false;
    D3DLOCKED_RECT lock{};
    if (FAILED(t.overlay->LockRect(0, &lock, nullptr, D3DLOCK_NO_DIRTY_UPDATE)))
        return false;
    // Repack live tiles into a compact atlas. The draw list references only
    // this upload's cells, so abandoned cells never leave visible trails.
    PackOverlay(plane, tiles, (uint32_t *)lock.pBits, lock.Pitch / 4, t.atlasColumns);
    if (FAILED(t.overlay->UnlockRect(0)))
        return false;
    RECT dirty{0, 0, t.atlasWidth, (LONG)((tiles.size() + t.atlasColumns - 1) / t.atlasColumns) * AtlasCellH};
    if (FAILED(t.overlay->AddDirtyRect(&dirty)))
        return false;
    t.overlayTiles = std::move(tiles);
    return true;
}

void AtlasVertices(Texture &t, const ScreenVertex (&quad)[4], float sx, float sy)
{
    t.overlayVertices.clear();
    t.overlayVertices.reserve(t.overlayTiles.size() * 6);
    const float sl = quad[1].u * t.width, st = quad[1].v * t.height;
    const float sr = quad[3].u * t.width, sb = quad[0].v * t.height;
    for (size_t i = 0; i < t.overlayTiles.size(); ++i)
    {
        const auto r = t.overlayTiles[i];
        const float l = std::max((float)r.left, sl), top = std::max((float)r.top, st);
        const float right = std::min((float)r.right, sr), bottom = std::min((float)r.bottom, sb);
        if (l >= right || top >= bottom)
            continue;
        const int ax = (int)(i % t.atlasColumns) * AtlasCellW + AtlasPad;
        const int ay = (int)(i / t.atlasColumns) * AtlasCellH + AtlasPad;
        const float x0 = quad[1].x + (l - sl) * sx, x1 = quad[1].x + (right - sl) * sx;
        const float y0 = quad[1].y + (top - st) * sy, y1 = quad[1].y + (bottom - st) * sy;
        const float u0 = (ax + (l - r.left) * 2) / t.atlasWidth, u1 = (ax + (right - r.left) * 2) / t.atlasWidth;
        const float v0 = (ay + (top - r.top) * 2) / t.atlasHeight, v1 = (ay + (bottom - r.top) * 2) / t.atlasHeight;
        const ScreenVertex a{x0, y0, 0, 1, u0, v0}, b{x1, y0, 0, 1, u1, v0}, c{x0, y1, 0, 1, u0, v1},
            d{x1, y1, 0, 1, u1, v1};
        t.overlayVertices.insert(t.overlayVertices.end(), {a, b, c, b, d, c});
    }
}
} // namespace vt::Presentation32::detail
