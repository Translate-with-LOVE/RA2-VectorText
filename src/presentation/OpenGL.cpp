// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "State.h"

// OpenGL BGRA8 texture uploads; preserve pixel-unpack state.
namespace vt::Presentation32::detail
{
// OpenGL 1.1 entry points are exported by opengl32; extension loading is
// irrelevant for the upload itself. Restore the unpack row length and
// alignment changed by our BGRA8 upload.
using TexImage = void(APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
using TexSub = void(APIENTRY *)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
using GetInt = void(APIENTRY *)(GLenum, GLint *);
using PixelStore = void(APIENTRY *)(GLenum, GLint);
using GetError = GLenum(APIENTRY *)();
TexImage realImage = nullptr;
TexSub realSub = nullptr;
GetInt glGetInt = nullptr;
PixelStore glStore = nullptr;
GetError glError = nullptr;
void APIENTRY HookImage(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format,
                        GLenum type, const void *pixels)
{
    if (state->enabled && glError && FromCnc(_ReturnAddress()) && target == GL_TEXTURE_2D && level == 0 &&
        format == GL_RGB && type == 0x8363 && !pixels)
    {
        realImage(target, level, GL_RGBA8, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        if (glError() == GL_NO_ERROR)
        {
            Guard guard;
            state->stats.backend = 3;
            return;
        }
        Disable("OpenGL BGRA8 texture unavailable");
    }
    realImage(target, level, internal, w, h, border, format, type, pixels);
}
void APIENTRY HookSub(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,
                      const void *pixels)
{
    std::vector<uint32_t> out;
    if (FromCnc(_ReturnAddress()) && target == GL_TEXTURE_2D && level == 0 && format == GL_RGB && type == 0x8363 &&
        pixels && glGetInt && glStore)
    {
        GLint row = 0, skipRows = 0, skipPixels = 0, alignment = 0, swap = 0;
        glGetInt(GL_UNPACK_ROW_LENGTH, &row);
        glGetInt(GL_UNPACK_SKIP_ROWS, &skipRows);
        glGetInt(GL_UNPACK_SKIP_PIXELS, &skipPixels);
        glGetInt(GL_UNPACK_ALIGNMENT, &alignment);
        glGetInt(GL_UNPACK_SWAP_BYTES, &swap);
        {
            Guard guard;
            auto it = state->buffers.find((void *)pixels);
            if (it != state->buffers.end() && w == it->second->width && h == it->second->height && !skipRows &&
                !skipPixels && !swap &&
                ((!row && ((w * 2 + alignment - 1) / alignment * alignment) == it->second->pitch * 2) ||
                 row == it->second->pitch))
            {
                out.resize((size_t)w * h);
                it->second->plane.ValidateNative((const unsigned short *)pixels, it->second->pitch);
                it->second->plane.CompositeRect(it->second->base, it->second->pitch, out.data(), w, {0, 0, w, h});
                RecordFrame(out.data(), w, h, w);
            }
        }
        if (!out.empty())
        {
            glStore(GL_UNPACK_ROW_LENGTH, 0);
            glStore(GL_UNPACK_ALIGNMENT, 4);
            realSub(target, level, x, y, w, h, 0x80E1, GL_UNSIGNED_BYTE, out.data());
            glStore(GL_UNPACK_ROW_LENGTH, row);
            glStore(GL_UNPACK_ALIGNMENT, alignment);
            return;
        }
    }
    realSub(target, level, x, y, w, h, format, type, pixels);
}
bool InstallOpenGL()
{
    HMODULE ogl = LoadLibraryW(L"opengl32.dll");
    if (ogl)
    {
        glGetInt = (GetInt)realProc(ogl, "glGetIntegerv");
        glStore = (PixelStore)realProc(ogl, "glPixelStorei");
        glError = (GetError)realProc(ogl, "glGetError");
        const bool image = Detour(ogl, "glTexImage2D", (void *)HookImage, (void **)&realImage);
        const bool sub = Detour(ogl, "glTexSubImage2D", (void *)HookSub, (void **)&realSub);
        // Both are needed: promoting allocation without converting later
        // uploads would produce a texture missing the retained text.
        if (!image || !sub)
        {
            Log::Note("Present32: OpenGL upload detours unavailable; retaining RGB565");
            return false;
        }
    }
    return true;
}
} // namespace vt::Presentation32::detail
