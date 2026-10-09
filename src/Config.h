// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace vt
{
// Configuration (VectorText.ini, loaded once on demand independently of logging)
namespace Cfg
{
enum Mode
{
    Mode_Off = 0,
    Mode_Observe = 1,
    Mode_Draw = 2
};

void Load(); // idempotent; called by named rendering getters
int Mode();
const char *FontFile();
int FontWeight();
int FontSizeLatin();
int FontSizeCJK();
int BaselineRow();
bool FitToAdvance();
bool AntiAlias();
bool Probe();                  // log each of the first eight distinct BitFont addresses once
int StemDarkening();           // >0 enables the autofitter's stem-darkening property
double Gamma();                // coverage gamma for the AA path
bool VectorMetrics();          // true: our own advances (natural metrics)
double AdvanceScale();         // Metrics=scaled: advance = game * scale
int Supersample();             // 1 = direct raster; 2/4 = larger raster, box-downsampled
bool LinearBlend();            // linear-light blending in RGB565 and presentation paths
bool Dither();                 // ordered dither on RGB565 output; BGRA8 stays undithered
int Outline();                 // 0 disables outline; positive values enable a fixed 3x3 pass
unsigned short OutlineColor(); // outline colour encoded as an RGB565 word

bool LegacyCodepage1252(); // render C1 slots using legacy CP1252 glyphs
bool HiDPI();              // D3D9 text follows the actual viewport scale using a 2x raster
bool ConfigBool(const char *key, bool def);
int ConfigInt(const char *key, int def);
void ConfigStr(const char *key, const char *def, char *out, int cch);
} // namespace Cfg
} // namespace vt
