// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "State.h"

namespace vt::Presentation32::detail
{
// Load per current context: core exports and WGL extension addresses are not
// interchangeable across drivers/contexts. Keep Windows 7 imports unchanged.
// clang-format off
#define VT_GL_API(X) \
    X(GetIntegerv, void, (GLenum, GLint*)) \
    X(GetBooleanv, void, (GLenum, GLboolean*)) \
    X(IsEnabled, GLboolean, (GLenum)) \
    X(Enable, void, (GLenum)) \
    X(Disable, void, (GLenum)) \
    X(ColorMask, void, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(GetString, const GLubyte*, (GLenum)) \
    X(ActiveTexture, void, (GLenum)) \
    X(BindTexture, void, (GLenum, GLuint)) \
    X(GenTextures, void, (GLsizei, GLuint*)) \
    X(DeleteTextures, void, (GLsizei, const GLuint*)) \
    X(TexParameteri, void, (GLenum, GLenum, GLint)) \
    X(PixelStorei, void, (GLenum, GLint)) \
    X(CopyTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
    X(GetTexLevelParameteriv, void, (GLenum, GLint, GLenum, GLint*)) \
    X(ReadBuffer, void, (GLenum)) \
    X(CreateShader, GLuint, (GLenum)) \
    X(ShaderSource, void, (GLuint, GLsizei, const char* const*, const GLint*)) \
    X(CompileShader, void, (GLuint)) \
    X(GetShaderiv, void, (GLuint, GLenum, GLint*)) \
    X(DeleteShader, void, (GLuint)) \
    X(CreateProgram, GLuint, ()) \
    X(AttachShader, void, (GLuint, GLuint)) \
    X(BindAttribLocation, void, (GLuint, GLuint, const char*)) \
    X(LinkProgram, void, (GLuint)) \
    X(GetProgramiv, void, (GLuint, GLenum, GLint*)) \
    X(DeleteProgram, void, (GLuint)) \
    X(UseProgram, void, (GLuint)) \
    X(GetUniformLocation, GLint, (GLuint, const char*)) \
    X(Uniform1i, void, (GLint, GLint)) \
    X(Uniform4f, void, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(GenBuffers, void, (GLsizei, GLuint*)) \
    X(BindBuffer, void, (GLenum, GLuint)) \
    X(BufferData, void, (GLenum, ptrdiff_t, const void*, GLenum)) \
    X(DeleteBuffers, void, (GLsizei, const GLuint*)) \
    X(GenVertexArrays, void, (GLsizei, GLuint*)) \
    X(BindVertexArray, void, (GLuint)) \
    X(DeleteVertexArrays, void, (GLsizei, const GLuint*)) \
    X(EnableVertexAttribArray, void, (GLuint)) \
    X(VertexAttribPointer, void, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(DrawArrays, void, (GLenum, GLint, GLsizei)) \
    X(GetAttribLocation, GLint, (GLuint, const char*)) \
    X(GetVertexAttribiv, void, (GLuint, GLenum, GLint*)) \
    X(GetVertexAttribPointerv, void, (GLuint, GLenum, void**)) \
    X(GetBufferSubData, void, (GLenum, ptrdiff_t, ptrdiff_t, void*))
// clang-format on
struct GlApi
{
#define DECLARE_GL(name, result, args)                                                                                 \
    using name##Proc = result(APIENTRY *) args;                                                                        \
    name##Proc name = nullptr;
    VT_GL_API(DECLARE_GL)
#undef DECLARE_GL
    bool Load(HMODULE module, FARPROC(WINAPI *extension)(LPCSTR))
    {
#define LOAD_GL(name, result, args)                                                                                    \
    name = (name##Proc)realProc(module, "gl" #name);                                                                   \
    if (!name)                                                                                                         \
    {                                                                                                                  \
        auto p = extension("gl" #name);                                                                                \
        if ((uintptr_t)p > 3 && (uintptr_t)p != (uintptr_t)-1)                                                         \
            name = (name##Proc)p;                                                                                      \
    }                                                                                                                  \
    if (!name)                                                                                                         \
        return false;
        VT_GL_API(LOAD_GL)
#undef LOAD_GL
        return true;
    }
};
#undef VT_GL_API
constexpr GLenum GlTexture0 = 0x84C0, GlTexture1 = 0x84C1, GlActiveTexture = 0x84E0;
constexpr GLenum GlCurrentProgram = 0x8B8D, GlVertexArray = 0x85B5, GlArrayBuffer = 0x8892, GlArrayBinding = 0x8894;
constexpr GLenum GlUnpackBuffer = 0x88EC, GlUnpackBinding = 0x88EF;
constexpr GLenum GlVertexShader = 0x8B31, GlFragmentShader = 0x8B30, GlCompileStatus = 0x8B81, GlLinkStatus = 0x8B82;
constexpr GLenum GlDynamicDraw = 0x88E8, GlFramebufferBinding = 0x8CA6, GlFramebufferSrgb = 0x8DB9;
constexpr GLenum GlClampEdge = 0x812F, GlBgra = 0x80E1;

struct GlScope
{
    GlApi &gl;
    GLint program = 0, vao = 0, array = 0, active = 0, textures[2]{}, unpack = 0, read = 0;
    GLint row = 0, alignment = 0, skipRows = 0, skipPixels = 0, swap = 0;
    GLboolean mask[4]{};
    GLboolean enabled[6]{};
    static constexpr GLenum switches[] = {GL_BLEND,        GL_DEPTH_TEST, GL_STENCIL_TEST,
                                          GL_SCISSOR_TEST, GL_CULL_FACE,  GlFramebufferSrgb};
    explicit GlScope(GlApi &api) : gl(api)
    {
        gl.GetIntegerv(GlCurrentProgram, &program);
        gl.GetIntegerv(GlVertexArray, &vao);
        gl.GetIntegerv(GlArrayBinding, &array);
        gl.GetIntegerv(GlActiveTexture, &active);
        for (int i = 0; i < 2; ++i)
        {
            gl.ActiveTexture(GlTexture0 + i);
            gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i]);
        }
        gl.ActiveTexture(active);
        gl.GetIntegerv(GlUnpackBinding, &unpack);
        gl.GetIntegerv(GL_READ_BUFFER, &read);
        gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
        gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        gl.GetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
        gl.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
        gl.GetIntegerv(GL_UNPACK_SWAP_BYTES, &swap);
        gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
        for (int i = 0; i < 6; ++i)
            enabled[i] = gl.IsEnabled(switches[i]);
    }
    void Unpack()
    {
        gl.BindBuffer(GlUnpackBuffer, 0);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        gl.PixelStorei(GL_UNPACK_SWAP_BYTES, 0);
    }
    ~GlScope()
    {
        gl.UseProgram(program);
        gl.BindVertexArray(vao);
        gl.BindBuffer(GlArrayBuffer, array);
        for (int i = 0; i < 2; ++i)
        {
            gl.ActiveTexture(GlTexture0 + i);
            gl.BindTexture(GL_TEXTURE_2D, textures[i]);
        }
        gl.ActiveTexture(active);
        gl.BindBuffer(GlUnpackBuffer, unpack);
        gl.ReadBuffer(read);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, row);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        gl.PixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
        gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
        gl.PixelStorei(GL_UNPACK_SWAP_BYTES, swap);
        gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
        for (int i = 0; i < 6; ++i)
            if (enabled[i])
                gl.Enable(switches[i]);
            else
                gl.Disable(switches[i]);
    }
};
} // namespace vt::Presentation32::detail
