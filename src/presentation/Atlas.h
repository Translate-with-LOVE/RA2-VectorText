// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "State.h"

namespace vt::Presentation32::detail
{
using CreateTexture = HRESULT(WINAPI *)(void *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, void **, HANDLE *);
bool AtlasShape(size_t count, int maxWidth, int maxHeight, int &width, int &height, int &columns);
void PackOverlay(const PixelPlane &plane, const std::vector<PixelRect> &tiles, uint32_t *output, int pitch,
                 int columns);
bool OutputScale(const Texture &t, const ScreenVertex (&vertices)[4], float &sx, float &sy);
bool ResizeAtlas(Texture &t, size_t count);
bool UploadOverlay(Texture &t, const PixelPlane &plane);
void AtlasVertices(Texture &t, const ScreenVertex (&quad)[4], float sx, float sy);
} // namespace vt::Presentation32::detail
