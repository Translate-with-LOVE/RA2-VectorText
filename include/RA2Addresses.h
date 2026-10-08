// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
// Native game.exe 1.006TUC profile, verified against Steam CRC 574DFF5D.
#pragma once

namespace ra2a
{
constexpr unsigned int kExeSize = 0x004D7940u;
constexpr unsigned int kExeTimestamp = 0x3B1EBBEDu;
constexpr unsigned int kExeCRC = 0x574DFF5Du;

constexpr unsigned int BitFont_Instance = 0x0084E9E8u; // BitFont* built from GAME.FNT
constexpr unsigned int BitText_Instance = 0x0084E9D0u; // BitText* singleton

constexpr unsigned int Drawing_GetTextDimensions = 0x00497360u;
constexpr unsigned int Drawing_GetTextDimensionsSz = 10;
constexpr unsigned int Drawing_TextDimensionsDone = 0x004973C0u;
constexpr unsigned int Drawing_TextDimensionsDoneSz = 5;
constexpr unsigned int Message_Background = 0x00600A47u;
constexpr unsigned int Message_BackgroundSz = 8;

constexpr unsigned int BitFont_GetTextDimension = 0x004314B0u;
constexpr unsigned int BitFont_GetTextDimensionSz = 6;
constexpr unsigned int BitFont_DimensionDone = 0x0043163Fu;
constexpr unsigned int BitFont_DimensionDoneSz = 6;

constexpr unsigned int Drawing_PrintUnicode = 0x00497B40u;
constexpr unsigned int Drawing_PrintUnicodeSz = 10;

constexpr unsigned int BitText_Print = 0x00432350u;
constexpr unsigned int BitText_PrintSz = 5;

constexpr unsigned int BitText_DrawText = 0x00432490u;
constexpr unsigned int BitText_DrawTextSz = 5;

constexpr unsigned int Subtitle_DrawDone = 0x0069B63Du;
constexpr unsigned int Subtitle_DrawDoneSz = 5;

constexpr unsigned int BitFont_Blit = 0x004318E0u;
constexpr unsigned int BitFont_BlitSz = 5;

constexpr unsigned int BitFont_DrawString = 0x00431CC0u;
constexpr unsigned int BitFont_DrawStringSz = 7;
constexpr unsigned int BitText_LineBreak = 0x00432551u;
constexpr unsigned int BitText_LineWrap = 0x00432763u;
constexpr unsigned int BitText_LineLast = 0x0043294Eu;
constexpr unsigned int BitText_LineSz = 6;
constexpr unsigned int BitFont_Unlock = 0x00432150u;
constexpr unsigned int BitFont_UnlockSz = 7;
constexpr unsigned int Drawing_LineBox = 0x00497956u;
constexpr unsigned int Drawing_LineBoxSz = 6;

constexpr const char *ExeName = "game.exe";
constexpr const char *DllName = "VectorText.dll";
constexpr const char *Version = "Red Alert 2 1.006TUC";
constexpr unsigned int Width_Helper = 0x00431690;
constexpr unsigned int Width_Return = 0x004316A6;
constexpr unsigned int Drawing_WidthReturn = 0x00497376;
constexpr unsigned int Loading_WidthReturn1 = 0x0053601E;
constexpr unsigned int Loading_WidthReturn2 = 0x00536071;
constexpr unsigned int Startup_WidthReturn1 = 0x00516774;
constexpr unsigned int Startup_WidthReturn2 = 0x005167CF;
constexpr unsigned int Message_WidthReturn = 0x00600A31;
constexpr unsigned int Tooltip_WidthReturn = 0x0046E32B;
constexpr unsigned int Tooltip_DrawReturn = 0x0046E461;
constexpr unsigned int Sidebar_WidthReturn = 0x00680173;
constexpr unsigned int String_BlitReturn = 0x00431E0D;
constexpr unsigned int Break_BlitReturn = 0x00432666;
constexpr unsigned int Wrap_BlitReturn = 0x004328A1;
constexpr unsigned int Last_BlitReturn = 0x00432A7A;
constexpr unsigned int Print_StringReturn = 0x0043238F;
constexpr unsigned int Ordinary_WidthReturn = 0x00497871;
constexpr unsigned int DSurface_Fill = 0x004AB520;
constexpr unsigned int DSurface_FillSlot = 0x007A0BF4;
constexpr unsigned int DSurface_Type = 0x004B14E0;
constexpr unsigned int DSurface_TypeSlot = 0x007A0C68;
constexpr unsigned int XSurface_Copy = 0x00434AE0;
constexpr unsigned int BSurface_Vtable = 0x0079AEF8;
constexpr unsigned int BSurface_Delete = 0x004113D0;
constexpr unsigned int Copier_Opaque = 0x007AFC84;
constexpr unsigned int Copier_Keyed = 0x007AFCA4;
} // namespace ra2a
