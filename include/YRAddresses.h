#pragma once

// ===========================================================================
//  gamemd.exe 1.001 (Yuri's Revenge, "UC" build) -- addresses used by M0
//
//  Every entry below was verified by disassembling the local gamemd.exe
//  (see game.fnt-加载机制研究.md in the game root).  Expected executable
//  identity as reported by Syringe:
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
    //     [esp+4..] = Point2D location, WORD flags, int marginX, int marginY
    // prologue: 53 55 56 57              push ebx/ebp/esi/edi          (4 bytes)
    //           8B 3D D0 C4 89 00        mov edi,[0089C4D0]           (6) -> 10
    constexpr unsigned int Drawing_GetTextDimensions    = 0x004A59E0u;
    constexpr unsigned int Drawing_GetTextDimensionsSz  = 10;

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
    // All original outputs are written; ESP+20h is the original entry stack.
    constexpr unsigned int BitFont_DimensionDone         = 0x00433E7Fu;
    constexpr unsigned int BitFont_DimensionDoneSz       = 6;

    // --- text drawing --------------------------------------------------------
    //
    // Drawing::PrintUnicode  (__cdecl-ish, args on stack; the string lives in
    // one of the first stack arguments -- M0 logs the candidates)
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

    // BitFont::Blit  (__thiscall)  -- hottest path: once per glyph
    //     ECX = BitFont* this
    //     [esp+4] = wchar_t wch  [esp+8] = int X  [esp+0xC] = int Y  [esp+0x10] = int nColor
    // prologue: 83 EC 30 / 53 / 55 / 56 / 57                           (7) -> 5
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
    // PrintUnicode's final Print call: mov ecx,[0089C4B8] (6 bytes).
    // EDI=viewport RectangleStruct; ESI=legacy X; EBP=Y; EBX=font.
    constexpr unsigned int Drawing_LineBox              = 0x004A5FD6u;
    constexpr unsigned int Drawing_LineBoxSz            = 6;
}
