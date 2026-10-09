// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../PixelPlaneTypes.h"
#include <cstddef>

namespace vt { class PixelPlane; }

namespace vt::Presentation32::detail
{
bool AtlasShape(size_t count, int maxWidth, int maxHeight, int &width, int &height, int &columns,int scale=2);
void PackOverlay(const PixelPlane &plane, const std::vector<PixelRect> &tiles, uint32_t *output, int pitch,
                 int columns,int scale=2);
} // namespace vt::Presentation32::detail
