// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
// field, handler, span, verified YR bytes, verified RA2 bytes.
VT_RUNTIME_HOOK(BitFont_Blit, VT_Hook_BitFont_Blit, BitFont_BlitSz, "\x83\xEC\x30\x53\x55", "\x83\xEC\x30\x53\x55")
VT_RUNTIME_HOOK(BitFont_DrawString, VT_Hook_BitFont_DrawString, BitFont_DrawStringSz, "\x83\xEC\x10\x8B\x54\x24\x14", "\x83\xEC\x10\x8B\x54\x24\x14")
VT_RUNTIME_HOOK(BitText_LineBreak, VT_Hook_BitText_LineBreak, BitText_LineSz, "\x8B\x4C\x24\x50\x03\xC1", "\x8B\x4C\x24\x50\x03\xC1")
VT_RUNTIME_HOOK(BitText_LineWrap, VT_Hook_BitText_LineWrap, BitText_LineSz, "\x8B\x54\x24\x50\x03\xC2", "\x8B\x54\x24\x50\x03\xC2")
VT_RUNTIME_HOOK(BitText_LineLast, VT_Hook_BitText_LineLast, BitText_LineSz, "\x8B\x54\x24\x50\x03\xC2", "\x8B\x54\x24\x50\x03\xC2")
VT_RUNTIME_HOOK(BitFont_Unlock, VT_Hook_BitFont_Unlock, BitFont_UnlockSz, "\x56\x8B\xF1\x8B\x4C\x24\x08", "\x56\x8B\xF1\x8B\x4C\x24\x08")
VT_RUNTIME_HOOK(Drawing_LineBox, VT_Hook_Drawing_LineBox, Drawing_LineBoxSz, "\x8B\x0D\xB8\xC4\x89\x00", "\x8B\x0D\xD0\xE9\x84\x00")
VT_RUNTIME_HOOK(Drawing_GetTextDimensions, VT_Hook_Drawing_GetTextDimensions, Drawing_GetTextDimensionsSz, "\x53\x55\x56\x57\x8B\x3D\xD0\xC4\x89\x00", "\x53\x55\x56\x57\x8B\x3D\xE8\xE9\x84\x00")
VT_RUNTIME_HOOK(Drawing_TextDimensionsDone, VT_Hook_Drawing_TextDimensionsDone, Drawing_TextDimensionsDoneSz, "\x89\x7B\x0C\x5F\x5E", "\x89\x7B\x0C\x5F\x5E")
VT_RUNTIME_HOOK(Message_Background, VT_Hook_Message_Background, Message_BackgroundSz, "\x89\x54\x24\x30\x8D\x44\x24\x30", "\x89\x54\x24\x30\x8D\x44\x24\x30")
VT_RUNTIME_HOOK(BitFont_GetTextDimension, VT_Hook_BitFont_GetTextDimension, BitFont_GetTextDimensionSz, "\x83\xEC\x1C\x8B\x41\x04", "\x83\xEC\x1C\x8B\x41\x04")
VT_RUNTIME_HOOK(BitFont_DimensionDone, VT_Hook_BitFont_DimensionDone, BitFont_DimensionDoneSz, "\xB0\x01\x5B\x83\xC4\x1C", "\xB0\x01\x5B\x83\xC4\x1C")
VT_RUNTIME_HOOK(Drawing_PrintUnicode, VT_Hook_Drawing_PrintUnicode, Drawing_PrintUnicodeSz, "\x8B\x44\x24\x08\x81\xEC\x0C\x04\x00\x00", "\x8B\x44\x24\x08\x81\xEC\x0C\x04\x00\x00")
VT_RUNTIME_HOOK(BitText_Print, VT_Hook_BitText_Print, BitText_PrintSz, "\x56\x8B\x74\x24\x08", "\x56\x8B\x74\x24\x08")
VT_RUNTIME_HOOK(BitText_DrawText, VT_Hook_BitText_DrawText, BitText_DrawTextSz, "\x83\xEC\x30\x53\x55", "\x83\xEC\x30\x53\x55")
VT_RUNTIME_HOOK(Subtitle_DrawDone, VT_Hook_Subtitle_DrawDone, Subtitle_DrawDoneSz, "\x8B\xD5\x83\xC3\x24", "\x8B\xD5\x83\xC3\x24")
