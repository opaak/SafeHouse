// See post.h.

#include "post.h"

#include "display.h"
#include "gl.h"

#include <algorithm>
#include <cstdio>

namespace
{
#if defined(__SWITCH__) || defined(__APPLE__)
constexpr bool kfAvailable = false;
#else
constexpr bool kfAvailable = true;
#endif

// A program and its uniforms; built at first use (state: 0 not yet, 1 built, -1 failed: the pass stays off).
struct POSTPROG
{
    GLSHADER sh;
    int state = 0;
    GLint tex = -1, rcpSize = -1, sharpness = -1, srcSize = -1, dstSize = -1;
    bool FReady(const char* frag)
    {
        if (state == 0)
        {
            state = sh.FInit("post.vert", frag, "") ? 1 : -1;
            if (state < 0)
            {
                std::printf("image quality: %s did not build: that pass is off\n", frag);
                return false;
            }
            tex = sh.Loc("tex");
            rcpSize = sh.Loc("rcpSize");
            sharpness = sh.Loc("sharpness");
            srcSize = sh.Loc("srcSize");
            dstSize = sh.Loc("dstSize");
        }
        return state > 0;
    }
};
POSTPROG s_fxaa, s_sharpen, s_scale;
GLuint s_vao = 0, s_sampler = 0;

// Two colour targets at the scene's size: the resolved copy the first pass reads, and the pass between FXAA and the
// sharpening.
struct TARGET { GLuint fbo = 0, tex = 0; };
TARGET s_atarget[2];
int s_dxTarget = 0, s_dyTarget = 0;

void FreeTargets()
{
    for (TARGET& t : s_atarget)
    {
        if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
        if (t.tex) glDeleteTextures(1, &t.tex);
        t = TARGET{};
    }
    s_dxTarget = s_dyTarget = 0;
}

void EnsureTargets(int dx, int dy)
{
    if (dx == s_dxTarget && dy == s_dyTarget && s_atarget[0].fbo) return;
    FreeTargets();
    for (TARGET& t : s_atarget)
    {
        glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dx, dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    }
    s_dxTarget = dx;
    s_dyTarget = dy;
}

void EnsureCommon()
{
    if (!s_vao) glGenVertexArrays(1, &s_vao);
    if (!s_sampler)
    {
        glGenSamplers(1, &s_sampler);
        glSamplerParameteri(s_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}

// The GL state the passes change, saved and put back (the scene's and the HUD's code keep their own caches of it).
struct GLSAVE
{
    GLint prog = 0, vao = 0, fboDraw = 0, fboRead = 0, activeTex = 0, tex0 = 0, sampler0 = 0;
    GLint viewport[4] = {};
    GLboolean fScissor = 0, fBlend = 0, fDepth = 0, fStencil = 0, fCull = 0, fDepthMask = 0;
    GLboolean colorMask[4] = {};
    GLSAVE()
    {
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboDraw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fboRead);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler0);
        glGetIntegerv(GL_VIEWPORT, viewport);
        fScissor = glIsEnabled(GL_SCISSOR_TEST);
        fBlend = glIsEnabled(GL_BLEND);
        fDepth = glIsEnabled(GL_DEPTH_TEST);
        fStencil = glIsEnabled(GL_STENCIL_TEST);
        fCull = glIsEnabled(GL_CULL_FACE);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &fDepthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindVertexArray(s_vao);
        glBindSampler(0, s_sampler);
    }
    ~GLSAVE()
    {
        auto set = [](GLenum cap, GLboolean f) { if (f) glEnable(cap); else glDisable(cap); };
        glUseProgram(GLuint(prog));
        glBindVertexArray(GLuint(vao));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fboDraw));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(fboRead));
        glBindTexture(GL_TEXTURE_2D, GLuint(tex0));
        glBindSampler(0, GLuint(sampler0));
        glActiveTexture(GLenum(activeTex));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        set(GL_SCISSOR_TEST, fScissor);
        set(GL_BLEND, fBlend);
        set(GL_DEPTH_TEST, fDepth);
        set(GL_STENCIL_TEST, fStencil);
        set(GL_CULL_FACE, fCull);
        glDepthMask(fDepthMask);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    }
};

void DrawFullScreen(GLuint texSource, GLuint fboDest, int x, int y, int dx, int dy)
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboDest);
    glViewport(x, y, dx, dy);
    glBindTexture(GL_TEXTURE_2D, texSource);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}
}  // namespace

void PostProcessScene()
{
    if (!kfAvailable || !g_gl.window) return;
    const DISPLAY& d = g_display;
    const bool fFxaa = d.postAa == 1 && s_fxaa.FReady("fxaa.frag");
    const bool fSharpen = d.upscaler == 1 && d.sharpness > 0 && s_sharpen.FReady("sharpen.frag");
    if (!fFxaa && !fSharpen) return;
    const int dx = g_gl.renderWidth, dy = g_gl.renderHeight;
    EnsureCommon();
    GLSAVE save;
    EnsureTargets(dx, dy);
    // The scene, resolved (MSAA) into the first target: the passes read it and write back into the scene target,
    // where the HUD and the foreground are drawn next.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.FboScene());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_atarget[0].fbo);
    glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLuint texIn = s_atarget[0].tex;
    if (fFxaa)
    {
        s_fxaa.sh.Use();
        glUniform1i(s_fxaa.tex, 0);
        glUniform2f(s_fxaa.rcpSize, 1.0f / float(dx), 1.0f / float(dy));
        const GLuint fboOut = fSharpen ? s_atarget[1].fbo : g_gl.FboScene();
        DrawFullScreen(texIn, fboOut, 0, 0, dx, dy);
        texIn = s_atarget[1].tex;
    }
    if (fSharpen)
    {
        s_sharpen.sh.Use();
        glUniform1i(s_sharpen.tex, 0);
        glUniform1f(s_sharpen.sharpness, float(std::clamp(d.sharpness, 0, 100)) / 100.0f);
        DrawFullScreen(texIn, g_gl.FboScene(), 0, 0, dx, dy);
    }
}

bool FPresentScaled(GLuint texScene, int dxSrc, int dySrc, int x, int y, int dx, int dy)
{
    if (!kfAvailable || g_display.upscaler != 1) return false;
    if (dxSrc == dx && dySrc == dy) return false;
    if (dx <= 0 || dy <= 0 || !s_scale.FReady("scale.frag")) return false;
    EnsureCommon();
    GLSAVE save;
    GLint fboDraw = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboDraw);
    s_scale.sh.Use();
    glUniform1i(s_scale.tex, 0);
    glUniform2f(s_scale.srcSize, float(dxSrc), float(dySrc));
    glUniform2f(s_scale.dstSize, float(dx), float(dy));
    DrawFullScreen(texScene, GLuint(fboDraw), x, y, dx, dy);
    return true;
}

void ShutdownPostProcess()
{
    if (!kfAvailable) return;
    FreeTargets();
    for (POSTPROG* p : {&s_fxaa, &s_sharpen, &s_scale})
        if (p->state > 0) p->sh.Delete(), p->state = 0;
    if (s_vao) glDeleteVertexArrays(1, &s_vao), s_vao = 0;
    if (s_sampler) glDeleteSamplers(1, &s_sampler), s_sampler = 0;
}
