// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GlOverlay.h"
#include "Atlas.h"
#include <string>

namespace vt::Presentation32::detail
{
namespace
{
using Context = HGLRC(WINAPI *)();
using Extension = FARPROC(WINAPI *)(LPCSTR);
using Swap = BOOL(WINAPI *)(HDC);
using DrawElements = void(APIENTRY *)(GLenum, GLsizei, GLenum, const void *);
using DeleteTextures = void(APIENTRY *)(GLsizei, const GLuint *);
using DeleteContext = BOOL(WINAPI *)(HGLRC);
using Image = void(APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
using SubImage = void(APIENTRY *)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
Context currentContext = nullptr;
Extension extension = nullptr;
Swap realSwap = nullptr;
DrawElements realDraw = nullptr;
DeleteTextures realDeleteTextures = nullptr;
DeleteContext realDeleteContext = nullptr;
Image image = nullptr;
SubImage subImage = nullptr;
HMODULE oglModule = nullptr;
struct Frame
{
    std::shared_ptr<const PixelPlane> plane;
    int width = 0, height = 0;
    bool clean = false;
    float left = 0, top = 0, right = 0, bottom = 0;
};
struct Output
{
    GlApi gl;
    bool loaded = false, failed = false;
    GLuint program = 0, vao = 0, vbo = 0, atlas = 0, scene = 0, selected = 0;
    int sceneW = 0, sceneH = 0, atlasW = 0, atlasH = 0, columns = 0, density = 2;
    GLint viewportUniform = -1;
    std::vector<PixelRect> tiles;
    std::map<GLuint, Frame> frames;
    std::shared_ptr<const PixelPlane> packed;
};
std::map<HGLRC, std::shared_ptr<Output>> contexts;
std::shared_ptr<Output> Current()
{
    const auto context = currentContext ? currentContext() : nullptr;
    if (!context)
        return nullptr;
    Guard guard;
    auto &output = contexts[context];
    if (!output)
        output = std::make_shared<Output>();
    if (!output->loaded && !output->failed)
    {
        output->loaded = output->gl.Load(oglModule, extension);
        if (!output->loaded)
        {
            output->failed = true;
            Log::Note("Present32: OpenGL overlay APIs unavailable; retaining logical BGRA8");
        }
    }
    return output->loaded ? output : nullptr;
}
GLuint Shader(GlApi &gl, GLenum type, const std::string &source)
{
    const auto shader = gl.CreateShader(type);
    const char *text = source.c_str();
    gl.ShaderSource(shader, 1, &text, nullptr);
    gl.CompileShader(shader);
    GLint compiled = 0;
    gl.GetShaderiv(shader, GlCompileStatus, &compiled);
    if (!compiled)
    {
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}
bool Program(Output &out)
{
    if (out.program)
        return true;
    if (out.failed)
        return false;
    auto &gl = out.gl;
    // Both core and compatibility profiles use the same explicit geometry.
    // Manual linear blending avoids depending on the window pixel format's
    // sRGB capability and never feeds text through the world upscaler.
    for (bool core : {true, false})
    {
        const std::string vertex = core ? "#version 150\nin vec2 Position;in vec2 TexCoord;out vec2 uv;void "
                                          "main(){uv=TexCoord;gl_Position=vec4(Position,0,1);}"
                                        : "#version 110\nattribute vec2 Position;attribute vec2 TexCoord;varying vec2 "
                                          "uv;void main(){uv=TexCoord;gl_Position=vec4(Position,0,1);}";
        const std::string sample = core ? "texture" : "texture2D";
        const std::string fragment =
            (core ? "#version 150\nin vec2 uv;out vec4 result;" : "#version 110\nvarying vec2 uv;") +
            std::string("uniform sampler2D Ink;uniform sampler2D Scene;uniform vec4 Viewport;uniform int "
                        "LinearBlend;void main(){vec4 ink=") +
            sample + "(Ink,uv);vec3 bg=" + sample +
            "(Scene,(gl_FragCoord.xy-Viewport.xy)/Viewport.zw).rgb;vec3 rgb;"
            "if(LinearBlend!=0){rgb=pow(max(vec3(0),ink.rgb+pow(bg,vec3(2.2))*(1.0-ink.a)),vec3(1.0/2.2));}"
            "else{rgb=ink.rgb+bg*(1.0-ink.a);}" +
            (core ? "result" : "gl_FragColor") + "=vec4(rgb,1);}";
        const auto vs = Shader(gl, GlVertexShader, vertex), fs = Shader(gl, GlFragmentShader, fragment);
        if (!vs || !fs)
        {
            if (vs)
                gl.DeleteShader(vs);
            if (fs)
                gl.DeleteShader(fs);
            continue;
        }
        const auto program = gl.CreateProgram();
        gl.AttachShader(program, vs);
        gl.AttachShader(program, fs);
        gl.BindAttribLocation(program, 0, "Position");
        gl.BindAttribLocation(program, 1, "TexCoord");
        gl.LinkProgram(program);
        gl.DeleteShader(vs);
        gl.DeleteShader(fs);
        GLint linked = 0;
        gl.GetProgramiv(program, GlLinkStatus, &linked);
        if (!linked)
        {
            gl.DeleteProgram(program);
            continue;
        }
        out.program = program;
        gl.UseProgram(program);
        gl.Uniform1i(gl.GetUniformLocation(program, "Ink"), 0);
        gl.Uniform1i(gl.GetUniformLocation(program, "Scene"), 1);
        gl.Uniform1i(gl.GetUniformLocation(program, "LinearBlend"), state->options.linear);
        out.viewportUniform = gl.GetUniformLocation(program, "Viewport");
        gl.GenVertexArrays(1, &out.vao);
        gl.GenBuffers(1, &out.vbo);
        gl.BindVertexArray(out.vao);
        gl.BindBuffer(GlArrayBuffer, out.vbo);
        gl.EnableVertexAttribArray(0);
        gl.EnableVertexAttribArray(1);
        gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
        gl.GenTextures(1, &out.atlas);
        gl.GenTextures(1, &out.scene);
        Log::Note("Present32: OpenGL independent text overlay ready (%s profile)", core ? "core" : "compatibility");
        return true;
    }
    out.failed = true;
    Log::Note("Present32: OpenGL overlay shader unavailable; retaining logical BGRA8");
    return false;
}
bool TextureStorage(Output &out, GLuint texture, int w, int h, GLint internal = GL_RGBA8)
{
    auto &gl = out.gl;
    GLint maximum = 0;
    gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (w <= 0 || h <= 0 || w > maximum || h > maximum)
        return false;
    gl.BindTexture(GL_TEXTURE_2D, texture);
    image(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GlClampEdge);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GlClampEdge);
    GLint actual = 0;
    gl.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actual);
    return actual == w;
}
bool Pack(Output &out, std::shared_ptr<const PixelPlane> plane)
{
    if (out.packed == plane)
        return true;
    const auto tiles = plane->TextTiles(true);
    if (tiles.empty())
    {
        out.tiles.clear();
        out.packed = std::move(plane);
        return true;
    }
    auto &gl = out.gl;
    GLint maximum = 0;
    gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    int width = 0, height = 0, columns = 0;
    const int n = plane->RasterScale();
    if (!AtlasShape(tiles.size(), maximum, maximum, width, height, columns, n))
        return false;
    gl.ActiveTexture(GlTexture0);
    if ((width != out.atlasW || height != out.atlasH) &&
        !TextureStorage(out, out.atlas, width, height, state->options.linear ? 0x8C43 : GL_RGBA8))
        return false;
    gl.BindTexture(GL_TEXTURE_2D, out.atlas);
    std::vector<uint32_t> pixels((size_t)width * height);
    PackOverlay(*plane, tiles, pixels.data(), width, columns, n);
    subImage(GL_TEXTURE_2D, 0, 0, 0, width, height, GlBgra, GL_UNSIGNED_BYTE, pixels.data());
    out.atlasW = width;
    out.atlasH = height;
    out.columns = columns;
    out.density = n;
    out.tiles = tiles;
    out.packed = std::move(plane);
    return true;
}
void Select(Output &out)
{
    auto &gl = out.gl;
    GLint active = 0, texture = 0;
    gl.GetIntegerv(GlActiveTexture, &active);
    gl.ActiveTexture(GlTexture0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    gl.ActiveTexture(active);
    auto found = out.frames.find(texture);
    if (found == out.frames.end())
        return;
    out.selected = texture;
    // Read the native primary quad's source UVs before its shader passes.
    // The first pass alone knows cropped video/upscale-hack source bounds.
    GLint program = 0;
    gl.GetIntegerv(GlCurrentProgram, &program);
    if (!program)
        return;
    const auto attr = gl.GetAttribLocation(program, "TexCoord");
    if (attr < 0)
        return;
    GLint buffer = 0, size = 0, type = 0, stride = 0, oldBuffer = 0;
    void *offset = nullptr;
    gl.GetVertexAttribiv(attr, 0x889F, &buffer);
    gl.GetVertexAttribiv(attr, 0x8623, &size);
    gl.GetVertexAttribiv(attr, 0x8625, &type);
    gl.GetVertexAttribiv(attr, 0x8624, &stride);
    gl.GetVertexAttribPointerv(attr, 0x8645, &offset);
    if (!buffer || size != 2 || type != GL_FLOAT || (stride && stride != 2 * sizeof(float)))
        return;
    gl.GetIntegerv(GlArrayBinding, &oldBuffer);
    gl.BindBuffer(GlArrayBuffer, buffer);
    float uv[8]{};
    gl.GetBufferSubData(GlArrayBuffer, (ptrdiff_t)offset, sizeof(uv), uv);
    gl.BindBuffer(GlArrayBuffer, oldBuffer);
    GLint tw = 0, th = 0;
    gl.ActiveTexture(GlTexture0);
    gl.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    gl.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    gl.ActiveTexture(active);
    float l = uv[0], r = l, t = uv[1], b = t;
    for (int i = 1; i < 4; ++i)
    {
        l = std::min(l, uv[2 * i]);
        r = std::max(r, uv[2 * i]);
        t = std::min(t, uv[2 * i + 1]);
        b = std::max(b, uv[2 * i + 1]);
    }
    auto &frame = found->second;
    if (l >= 0 && t >= 0 && r > l && b > t && r * tw <= frame.width + 0.01f && b * th <= frame.height + 0.01f)
    {
        frame.left = l * tw;
        frame.right = r * tw;
        frame.top = t * th;
        frame.bottom = b * th;
    }
}
void Present(Output &out)
{
    auto found = out.frames.find(out.selected);
    if (found == out.frames.end())
        return;
    auto &frame = found->second;
    auto &gl = out.gl;
    GLint viewport[4]{}, fbo = 0;
    gl.GetIntegerv(GL_VIEWPORT, viewport);
    gl.GetIntegerv(GlFramebufferBinding, &fbo);
    const float sx = viewport[2] / (frame.right - frame.left), sy = viewport[3] / (frame.bottom - frame.top);
    if (fbo)
        return;
    ObserveOutput(sx, sy, frame.clean ? frame.plane->RasterScale() : 0);
    // Keep matching text on a previously clean world during a resize,
    // including the transitional 1x frame before the next native upload.
    if (!frame.clean)
        return;
    GlScope scope(gl);
    scope.Unpack();
    if (!Pack(out, frame.plane) || out.tiles.empty())
        return;
    gl.ActiveTexture(GlTexture1);
    if (viewport[2] != out.sceneW || viewport[3] != out.sceneH)
    {
        if (!TextureStorage(out, out.scene, viewport[2], viewport[3]))
            return;
        out.sceneW = viewport[2];
        out.sceneH = viewport[3];
    }
    gl.BindTexture(GL_TEXTURE_2D, out.scene);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLint drawBuffer = 0;
    gl.GetIntegerv(GL_DRAW_BUFFER, &drawBuffer);
    gl.ReadBuffer(drawBuffer);
    // Copy only the union of visible text cells, not the complete 4K world.
    int cl = viewport[2], ct = viewport[3], cr = 0, cb = 0;
    std::vector<float> vertices;
    vertices.reserve(out.tiles.size() * 24);
    for (size_t i = 0; i < out.tiles.size(); ++i)
    {
        const auto r = out.tiles[i];
        const float l = std::max((float)r.left, frame.left), t = std::max((float)r.top, frame.top);
        const float right = std::min((float)r.right, frame.right), bottom = std::min((float)r.bottom, frame.bottom);
        if (l >= right || t >= bottom)
            continue;
        const float px = (l - frame.left) * sx, py = (t - frame.top) * sy, qx = (right - frame.left) * sx,
                    qy = (bottom - frame.top) * sy;
        cl = std::min(cl, (int)std::floor(px));
        ct = std::min(ct, (int)std::floor(py));
        cr = std::max(cr, (int)std::ceil(qx));
        cb = std::max(cb, (int)std::ceil(qy));
        const int ax = (int)(i % out.columns) * 34 * out.density + out.density,
                  ay = (int)(i / out.columns) * 18 * out.density + out.density;
        const float u0 = (ax + (l - r.left) * out.density) / out.atlasW,
                    u1 = (ax + (right - r.left) * out.density) / out.atlasW;
        const float v0 = (ay + (t - r.top) * out.density) / out.atlasH,
                    v1 = (ay + (bottom - r.top) * out.density) / out.atlasH;
        const float x0 = px * 2 / viewport[2] - 1, x1 = qx * 2 / viewport[2] - 1, y0 = 1 - py * 2 / viewport[3],
                    y1 = 1 - qy * 2 / viewport[3];
        const float quad[] = {x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1,
                              x0, y1, u0, v1, x1, y0, u1, v0, x1, y1, u1, v1};
        vertices.insert(vertices.end(), quad, quad + 24);
    }
    if (vertices.empty())
        return;
    cl = std::max(0, cl);
    ct = std::max(0, ct);
    cr = std::min(viewport[2], cr);
    cb = std::min(viewport[3], cb);
    gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, cl, viewport[3] - cb, viewport[0] + cl, viewport[1] + viewport[3] - cb,
                         cr - cl, cb - ct);
    gl.ActiveTexture(GlTexture0);
    gl.BindTexture(GL_TEXTURE_2D, out.atlas);
    const int filter =
        std::abs(sx - out.density) < 0.0001f && std::abs(sy - out.density) < 0.0001f ? GL_NEAREST : GL_LINEAR;
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    for (auto cap : GlScope::switches)
        gl.Disable(cap);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.UseProgram(out.program);
    gl.Uniform4f(out.viewportUniform, (float)viewport[0], (float)viewport[1], (float)viewport[2], (float)viewport[3]);
    gl.BindVertexArray(out.vao);
    gl.BindBuffer(GlArrayBuffer, out.vbo);
    gl.BufferData(GlArrayBuffer, (ptrdiff_t)(vertices.size() * sizeof(float)), vertices.data(), GlDynamicDraw);
    gl.DrawArrays(GL_TRIANGLES, 0, (GLsizei)(vertices.size() / 4));
    Guard guard;
    ++state->stats.overlays;
}
BOOL WINAPI HookSwap(HDC dc)
{
    if (state->enabled && state->autoTextScale && FromCnc(_ReturnAddress()))
    {
        auto out = Current();
        if (out && out->program)
            Present(*out);
    }
    return realSwap(dc);
}
void APIENTRY HookDraw(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    if (state->enabled && state->autoTextScale && FromCnc(_ReturnAddress()) && mode == GL_TRIANGLES && count == 6)
    {
        auto out = Current();
        if (out)
            Select(*out);
    }
    realDraw(mode, count, type, indices);
}
void APIENTRY HookDeleteTextures(GLsizei count, const GLuint *textures)
{
    auto out = Current();
    if (out)
        for (int i = 0; i < count; ++i)
        {
            out->frames.erase(textures[i]);
            if (out->selected == textures[i])
                out->selected = 0;
        }
    realDeleteTextures(count, textures);
}
BOOL WINAPI HookDeleteContext(HGLRC context)
{
    std::shared_ptr<Output> out;
    {
        Guard guard;
        auto found = contexts.find(context);
        if (found != contexts.end())
        {
            out = found->second;
            contexts.erase(found);
        }
    }
    if (out && out->loaded && currentContext() == context)
    {
        auto &gl = out->gl;
        if (out->program)
            gl.DeleteProgram(out->program);
        if (out->vao)
            gl.DeleteVertexArrays(1, &out->vao);
        if (out->vbo)
            gl.DeleteBuffers(1, &out->vbo);
        if (out->atlas)
            realDeleteTextures(1, &out->atlas);
        if (out->scene)
            realDeleteTextures(1, &out->scene);
    }
    return realDeleteContext(context);
}
} // namespace
bool StageGlFrame(GLuint texture, int width, int height, std::shared_ptr<const PixelPlane> frame)
{
    auto out = Current();
    if (!out)
        return false;
    auto &gl = out->gl;
    Frame next{frame, width, height, false, 0, 0, (float)width, (float)height};
    GLint viewport[4]{};
    gl.GetIntegerv(GL_VIEWPORT, viewport);
    if (ObserveOutput(viewport[2] / (float)width, viewport[3] / (float)height, frame->RasterScale()))
    {
        GlScope scope(gl);
        scope.Unpack();
        if (Program(*out))
        {
            gl.ActiveTexture(GlTexture1);
            if (viewport[2] != out->sceneW || viewport[3] != out->sceneH)
            {
                if (TextureStorage(*out, out->scene, viewport[2], viewport[3]))
                {
                    out->sceneW = viewport[2];
                    out->sceneH = viewport[3];
                }
            }
            next.clean = out->sceneW == viewport[2] && out->sceneH == viewport[3] && Pack(*out, frame);
        }
    }
    if (!next.clean)
        ObserveOutput(0, 0, 0);
    out->frames[texture] = next;
    return next.clean;
}
bool InstallGlOverlay(HMODULE module)
{
    oglModule = module;
    currentContext = (Context)realProc(module, "wglGetCurrentContext");
    extension = (Extension)realProc(module, "wglGetProcAddress");
    image = (Image)realProc(module, "glTexImage2D");
    subImage = (SubImage)realProc(module, "glTexSubImage2D");
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    return currentContext && extension && image && subImage &&
           Detour(gdi, "SwapBuffers", (void *)HookSwap, (void **)&realSwap) &&
           Detour(module, "glDrawElements", (void *)HookDraw, (void **)&realDraw) &&
           Detour(module, "glDeleteTextures", (void *)HookDeleteTextures, (void **)&realDeleteTextures) &&
           Detour(module, "wglDeleteContext", (void *)HookDeleteContext, (void **)&realDeleteContext);
}
} // namespace vt::Presentation32::detail
