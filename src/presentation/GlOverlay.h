// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "GlApi.h"

namespace vt::Presentation32::detail
{
bool InstallGlOverlay(HMODULE module);
bool StageGlFrame(GLuint texture, int width, int height, std::shared_ptr<const PixelPlane> frame);
} // namespace vt::Presentation32::detail
