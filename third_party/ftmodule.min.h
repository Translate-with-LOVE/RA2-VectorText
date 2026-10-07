// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
/*
 * Minimal FreeType module table for VectorText.
 *
 * Registered through -DFT_CONFIG_MODULES_H="ftmodule.min.h" so that the
 * library only knows about the modules we actually compile in:
 *
 *   autofit  - auto-hinter (matters a lot at 13 px)
 *   tt       - TrueType driver (glyf outlines; Noto Serif SC is TrueType)
 *   sfnt     - shared SFNT/OpenType table access used by the tt driver
 *   psnames  - PostScript name/cmap helpers required by sfnt
 *   smooth   - 8-bit coverage rasteriser (M2: anti-aliased text)
 *   raster1  - monochrome rasteriser   (M1: 1bpp text into 8bpp surfaces)
 *
 * Deliberately not registered: Type1/CFF/CID/PFR/Type42/WinFNT/PCF/BDF,
 * PS aux/hinter, SDF and SVG renderers.
 */

FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )

/* EOF */
