// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ===========================================================================
//  gamemd.exe 1.001 (Yuri's Revenge) -- native text hook addresses
//
//  Addresses and overwritten instruction spans are verified against the local
//  executable. See docs/rendering.md for the rendering paths and
//  tools/verify_dll.py for native instruction and hook-span checks. Expected identity
//  as reported by Syringe:
//      size = 0x00497FE0   timestamp = 0x3BDF544E   CRC = 0x1B499086
//
//  `hookSize` is the number of bytes Syringe saves/restores: it must be >= 5
//  (the jmp it writes) and must end on an instruction boundary, otherwise the
//  pass-through path would resume in the middle of an instruction.
// ===========================================================================

namespace yra
{
    // --- executable identity -------------------------------------------------
    constexpr unsigned int kExeSize      = 0x00497FE0u;
    constexpr unsigned int kExeTimestamp = 0x3BDF544Eu;
    constexpr unsigned int kExeCRC       = 0x1B499086u;

    // --- globals -------------------------------------------------------------
    constexpr unsigned int BitFont_Instance = 0x0089C4D0u;  // BitFont* built from GAME.FNT
    constexpr unsigned int BitText_Instance = 0x0089C4B8u;  // BitText* singleton

    // --- text measurement ----------------------------------------------------
    //
    // Drawing::GetTextDimensions  (__fastcall)
    //     ECX = RectangleStruct* pOutBuffer
    //     EDX = const wchar_t*   pText
    //     [esp+4] = location.X  [esp+8] = location.Y
    //     [esp+0xC] = flags (WORD in a DWORD stack slot)
    //     [esp+0x10] = marginX  [esp+0x14] = marginY
    // prologue: 53 55 56 57              push ebx/ebp/esi/edi          (4 bytes)
    //           8B 3D D0 C4 89 00        mov edi,[0089C4D0]           (6) -> 10
    constexpr unsigned int Drawing_GetTextDimensions    = 0x004A59E0u;
    constexpr unsigned int Drawing_GetTextDimensionsSz  = 10;
    // Final height: mov [ebx+0Ch],edi / pop edi / pop esi (3+1+1).
    // EBX = output RectangleStruct; EDI = height before replay.
    constexpr unsigned int Drawing_TextDimensionsDone   = 0x004A5A40u;
    constexpr unsigned int Drawing_TextDimensionsDoneSz = 5;
    // Message rectangle setup: mov [esp+30h],edx / lea eax,[esp+30h] (4+4).
    // RectangleStruct at entry ESP+30h is X/Y/W/H; replay writes X from EDX.
    // EBP supplies height at 623AA4 after two pushes. Phobos may replace that
    // fill path at 623A9F, so this earlier hook prepares both native and Phobos
    // fills without overlapping Phobos' hook span.
    constexpr unsigned int Message_Background           = 0x00623A97u;
    constexpr unsigned int Message_BackgroundSz         = 8;

    // BitFont::GetTextDimension  (__thiscall)
    //     ECX = BitFont* this
    //     [esp+4]  = const wchar_t* pText
    //     [esp+8]  = int* pWidth
    //     [esp+0xC]= int* pHeight
    //     [esp+0x10]= int nMaxWidth
    // prologue: 83 EC 1C                 sub esp,0x1C                  (3)
    //           8B 41 04                 mov eax,[ecx+4]               (3) -> 6
    constexpr unsigned int BitFont_GetTextDimension     = 0x00433CF0u;
    constexpr unsigned int BitFont_GetTextDimensionSz   = 6;
    // Successful measurement epilogue: mov al,1 / pop ebx / add esp,1Ch.
    // Original outputs are already written; entry ESP = current ESP+20h.
    // The return address is at [esp+20h], before replay pops EBX and locals.
    constexpr unsigned int BitFont_DimensionDone         = 0x00433E7Fu;
    constexpr unsigned int BitFont_DimensionDoneSz       = 6;

    // --- text drawing --------------------------------------------------------
    //
    // Drawing::PrintUnicode (__cdecl, formatted wide text)
    //     [esp+4] = Point2D* output  [esp+8] = const wchar_t* format
    //     [esp+0xC] = Surface*; remaining fixed arguments precede varargs.
    // The hook observes the format string; it does not replace formatting.
    // Drawing_LineBox below is in its downstream helper at 4A5EB0.
    // prologue: 8B 44 24 08              mov eax,[esp+8]               (4)
    //           81 EC 0C 04 00 00        sub esp,0x40C                 (6) -> 10
    constexpr unsigned int Drawing_PrintUnicode         = 0x004A61C0u;
    constexpr unsigned int Drawing_PrintUnicodeSz       = 10;

    // BitText::Print  (__thiscall, 7 stack args)
    //     ECX = BitText* this
    //     [esp+4] = BitFont*  [esp+8] = Surface*  [esp+0xC] = const wchar_t*
    //     [esp+0x10] = X  [esp+0x14] = Y  [esp+0x18] = W  [esp+0x1C] = H
    // prologue: 56                       push esi                      (1)
    //           8B 74 24 08              mov esi,[esp+8]               (4) -> 5
    constexpr unsigned int BitText_Print                = 0x00434B90u;
    constexpr unsigned int BitText_PrintSz              = 5;

    // BitText::DrawText  (__thiscall, 10 stack args)
    //     ECX = BitText* this
    //     [esp+4] = BitFont*  [esp+8] = Surface*  [esp+0xC] = const wchar_t*
    //     [esp+0x10] = X  [esp+0x14] = Y  [esp+0x18] = W  [esp+0x1C] = H
    //     [esp+0x20] = char a8  [esp+0x24] = int a9  [esp+0x28] = int nColorAdjust
    // prologue: 83 EC 30 / 53 / 55                                     (5) -> 5
    constexpr unsigned int BitText_DrawText             = 0x00434CD0u;
    constexpr unsigned int BitText_DrawTextSz           = 5;

    // Movie subtitle draw has returned; locals +10=X, +14=width, +24=height,
    // EDI=Y. Native code saves these as the next erase rectangle at object+14.
    // Replay: mov edx,ebp / add ebx,24h (2+3), before copying the old rectangle.
    constexpr unsigned int Subtitle_DrawDone            = 0x006C9E7Du;
    constexpr unsigned int Subtitle_DrawDoneSz          = 5;

    // BitFont::Blit  (__thiscall)  -- hottest path: once per glyph
    //     ECX = BitFont* this
    //     [esp+4] = wchar_t wch  [esp+8] = int X  [esp+0xC] = int Y  [esp+0x10] = int nColor
    // overwritten prefix: sub esp,30h / push ebx / push ebp (3+1+1).
    // push esi / push edi follow at 434125; they are outside this hook span.
    constexpr unsigned int BitFont_Blit                 = 0x00434120u;
    constexpr unsigned int BitFont_BlitSz               = 5;

    // Single-line X takeover; all sites verified against local gamemd.exe.
    // DrawString entry: sub esp,10h / mov edx,[esp+14h] (3+4).
    constexpr unsigned int BitFont_DrawString           = 0x00434500u;
    constexpr unsigned int BitFont_DrawStringSz         = 7;
    // DrawText has three row loops (explicit break / wrapped / final row).
    // Each site: mov reg,[esp+50h] / add eax,reg (4+2).
    constexpr unsigned int BitText_LineBreak            = 0x00434D91u;
    constexpr unsigned int BitText_LineWrap             = 0x00434FA3u;
    constexpr unsigned int BitText_LineLast             = 0x0043518Eu;
    constexpr unsigned int BitText_LineSz               = 6;
    // Unlock entry: push esi / mov esi,ecx / mov ecx,[esp+8] (1+2+4).
    constexpr unsigned int BitFont_Unlock               = 0x00434990u;
    constexpr unsigned int BitFont_UnlockSz             = 7;
    // Drawing helper at 4A5EB0, before its BitText::Print call at 4A5FED:
    // mov ecx,[0089C4B8] (6 bytes). This is not inside PrintUnicode at 4A61C0.
    // EDI=viewport RectangleStruct*; ESI=legacy X; EBP=Y; EBX=BitFont*.
    constexpr unsigned int Drawing_LineBox              = 0x004A5FD6u;
    constexpr unsigned int Drawing_LineBoxSz            = 6;

    constexpr const char* ExeName = "gamemd.exe";
    constexpr const char* DllName = "VectorText.dll";
    constexpr const char* Version = "Yuri's Revenge 1.001";
    constexpr unsigned int Width_Helper = 0x433ED0;
    constexpr unsigned int Width_Return = 0x433EE6;
    constexpr unsigned int Drawing_WidthReturn = 0x4A59F6;
    constexpr unsigned int Loading_WidthReturn1 = 0x553199;
    constexpr unsigned int Loading_WidthReturn2 = 0x5531EF;
    constexpr unsigned int Startup_WidthReturn1 = 0x531459;
    constexpr unsigned int Startup_WidthReturn2 = 0x5314B3;
    constexpr unsigned int Message_WidthReturn = 0x623A81;
    constexpr unsigned int Tooltip_WidthReturn = 0x478F0B;
    constexpr unsigned int Tooltip_DrawReturn = 0x479041;
    constexpr unsigned int Sidebar_WidthReturn = 0x6A9DD1;
    constexpr unsigned int String_BlitReturn = 0x43464D;
    constexpr unsigned int Break_BlitReturn = 0x434EA6;
    constexpr unsigned int Wrap_BlitReturn = 0x4350E1;
    constexpr unsigned int Last_BlitReturn = 0x4352BA;
    constexpr unsigned int Print_StringReturn = 0x434BCF;
    constexpr unsigned int Ordinary_WidthReturn = 0x4A5EF1;
    constexpr unsigned int DSurface_Fill = 0x4BB620;
    constexpr unsigned int DSurface_FillSlot = 0x7E85E4;
    constexpr unsigned int DSurface_Type = 0x4C1AB0;
    constexpr unsigned int DSurface_TypeSlot = 0x7E8658;
    constexpr unsigned int XSurface_Copy = 0x437350;
    constexpr unsigned int BSurface_Vtable = 0x7E2070;
    constexpr unsigned int BSurface_Delete = 0x411650;
    constexpr unsigned int Copier_Opaque = 0x7F7BC4;
    constexpr unsigned int Copier_Keyed = 0x7F7BF4;
}
