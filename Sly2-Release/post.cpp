// See post.h.

#include "post.h"

#include "disc.h"
#include "display.h"
#include "gl.h"
#include "render.h"
#include "texmod.h"
#include "../platform/platform.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

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
POSTPROG s_fxaa, s_sharpen, s_scale, s_outline, s_bloomDown, s_bloomUp, s_grade, s_ao, s_aoApply;
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
        glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);   // (the LUT)
    }
}

// The outlines' copy of the scene's depth (the scene's is a renderbuffer, multisampled with MSAA: a blit takes one
// sample per pixel), at the scene's size, read with texelFetch.
GLuint s_fboDepth = 0, s_texDepth = 0;
int s_dxDepth = 0, s_dyDepth = 0;

void FreeDepth()
{
    if (s_fboDepth) glDeleteFramebuffers(1, &s_fboDepth), s_fboDepth = 0;
    if (s_texDepth) glDeleteTextures(1, &s_texDepth), s_texDepth = 0;
    s_dxDepth = s_dyDepth = 0;
}

void EnsureDepth(int dx, int dy)
{
    if (dx == s_dxDepth && dy == s_dyDepth && s_fboDepth) return;
    FreeDepth();
    glGenTextures(1, &s_texDepth);
    glBindTexture(GL_TEXTURE_2D, s_texDepth);
    // (the scene's own format: a depth blit needs the same one)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8, dx, dy, 0, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
    glGenFramebuffers(1, &s_fboDepth);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fboDepth);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_texDepth, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    s_dxDepth = dx;
    s_dyDepth = dy;
}

// Ambient occlusion's openness (ao.frag), R8 at the scene's size.
GLuint s_fboAo = 0, s_texAo = 0;
int s_dxAo = 0, s_dyAo = 0;

void FreeAo()
{
    if (s_fboAo) glDeleteFramebuffers(1, &s_fboAo), s_fboAo = 0;
    if (s_texAo) glDeleteTextures(1, &s_texAo), s_texAo = 0;
    s_dxAo = s_dyAo = 0;
}

void EnsureAo(int dx, int dy)
{
    if (dx == s_dxAo && dy == s_dyAo && s_fboAo) return;
    FreeAo();
    glGenTextures(1, &s_texAo);
    glBindTexture(GL_TEXTURE_2D, s_texAo);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, dx, dy, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glGenFramebuffers(1, &s_fboAo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fboAo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_texAo, 0);
    s_dxAo = dx;
    s_dyAo = dy;
}

// The bloom chain: half the scene's size, then halving down to about 16 lines (at most kcBloomLevels), RGBA16F.
constexpr int kcBloomLevels = 6;
struct LEVEL { GLuint fbo = 0, tex = 0; int dx = 0, dy = 0; };
std::vector<LEVEL> s_abloom;
int s_dxBloom = 0, s_dyBloom = 0;

void FreeBloom()
{
    for (LEVEL& l : s_abloom)
    {
        if (l.fbo) glDeleteFramebuffers(1, &l.fbo);
        if (l.tex) glDeleteTextures(1, &l.tex);
    }
    s_abloom.clear();
    s_dxBloom = s_dyBloom = 0;
}

void EnsureBloom(int dx, int dy)
{
    if (dx == s_dxBloom && dy == s_dyBloom && !s_abloom.empty()) return;
    FreeBloom();
    int w = std::max(dx / 2, 1), h = std::max(dy / 2, 1);
    for (int i = 0; i < kcBloomLevels && (i == 0 || h >= 16); ++i, w = std::max(w / 2, 1), h = std::max(h / 2, 1))
    {
        LEVEL l;
        l.dx = w, l.dy = h;
        glGenTextures(1, &l.tex);
        glBindTexture(GL_TEXTURE_2D, l.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glGenFramebuffers(1, &l.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, l.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l.tex, 0);
        s_abloom.push_back(l);
    }
    s_dxBloom = dx;
    s_dyBloom = dy;
}

// Grade 2's LUT: Mods/ColorGrading/lut.png, a strip of N tiles of N x N (N*N wide, N high: red across a tile, green
// down it, blue from tile to tile; the layout ReShade's and Unity's LUTs use), N 16, 32 or 64. Read once; when it is
// missing, a neutral one (neutral.png, which changes nothing) is written next to it to edit in any image editor.
GLuint s_texLut = 0;
int s_stateLut = 0;   // 0 not yet, 1 loaded, -1 none (grade 2 then draws as the original)
int s_nLut = 0;

bool FEnsureLut()
{
    if (s_stateLut != 0) return s_stateLut > 0;
    s_stateLut = -1;
    const std::string& dirMods = g_disc.DirMods();
    if (dirMods.empty())
    {
        std::printf("colour grading: no mods folder, so no LUT: the original colours\n");
        return false;
    }
    const std::string dir = dirMods + "/ColorGrading";
    std::vector<uint8_t> ab;
    int w = 0, h = 0;
    if (!FLoadImageRgba(dir + "/lut.png", ab, &w, &h))
    {
        constexpr int n = 32;
        std::vector<uint8_t> abNeutral(size_t(n) * n * n * 4);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n * n; ++x)
            {
                uint8_t* pb = &abNeutral[(size_t(y) * n * n + x) * 4];
                pb[0] = uint8_t((x % n) * 255 / (n - 1));
                pb[1] = uint8_t(y * 255 / (n - 1));
                pb[2] = uint8_t((x / n) * 255 / (n - 1));
                pb[3] = 255;
            }
        std::error_code ec;
        const bool fNeutral = std::filesystem::exists(cane::path(dir + "/neutral.png"), ec) ||
                              FWritePngRgba(dir + "/neutral.png", abNeutral, n * n, n);
        std::printf("colour grading: no %s/lut.png: the original colours%s\n", dir.c_str(),
                    fNeutral ? " (neutral.png there is a LUT that changes nothing: edit it and save it as lut.png)" : "");
        return false;
    }
    const int n = h;
    if (!(n == 16 || n == 32 || n == 64) || w != n * n)
    {
        std::printf("colour grading: %s/lut.png is %dx%d, not a 256x16, 1024x32 or 4096x64 strip: the original colours\n",
                    dir.c_str(), w, h);
        return false;
    }
    std::vector<uint8_t> ab3(size_t(n) * n * n * 4);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
                std::copy_n(&ab[(size_t(g) * w + size_t(b) * n + r) * 4], 4, &ab3[((size_t(b) * n + g) * n + r) * 4]);
    glGenTextures(1, &s_texLut);
    glBindTexture(GL_TEXTURE_3D, s_texLut);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, n, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, ab3.data());
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindTexture(GL_TEXTURE_3D, 0);
    s_nLut = n;
    s_stateLut = 1;
    std::printf("colour grading: %s/lut.png (%d^3)\n", dir.c_str(), n);
    return true;
}

// The GL state the passes change, saved and put back (the scene's and the HUD's code keep their own caches of it).
struct GLSAVE
{
    GLint prog = 0, vao = 0, fboDraw = 0, fboRead = 0, activeTex = 0, tex0 = 0, sampler0 = 0;
    GLint tex1 = 0, sampler1 = 0, tex3d2 = 0, tex2d2 = 0, sampler2 = 0;   // (units 1 and 2: the depth, the AO, the bloom, the LUT)
    GLint blendSrcRgb = 0, blendDstRgb = 0, blendSrcAlpha = 0, blendDstAlpha = 0, blendEqRgb = 0, blendEqAlpha = 0;
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
        glActiveTexture(GL_TEXTURE2);
        glGetIntegerv(GL_TEXTURE_BINDING_3D, &tex3d2);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d2);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler2);
        glActiveTexture(GL_TEXTURE1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler1);
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
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEqRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqAlpha);
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
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_3D, GLuint(tex3d2));
        glBindTexture(GL_TEXTURE_2D, GLuint(tex2d2));
        glBindSampler(2, GLuint(sampler2));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, GLuint(tex1));
        glBindSampler(1, GLuint(sampler1));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, GLuint(tex0));
        glBindSampler(0, GLuint(sampler0));
        glActiveTexture(GLenum(activeTex));
        glBlendFuncSeparate(GLenum(blendSrcRgb), GLenum(blendDstRgb), GLenum(blendSrcAlpha), GLenum(blendDstAlpha));
        glBlendEquationSeparate(GLenum(blendEqRgb), GLenum(blendEqAlpha));
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

// The bloom chain from the resolved picture (texScene): down with the prefilter into the half-size level, down level by
// level, then each smaller level's tent added into the next larger one; the largest level then holds the glow.
void BuildBloom(GLuint texScene, int dx, int dy)
{
    EnsureBloom(dx, dy);
    s_bloomDown.sh.Use();
    glUniform1i(s_bloomDown.tex, 0);
    const GLint locPrefilter = s_bloomDown.sh.Loc("fPrefilter");
    glUniform1f(s_bloomDown.sh.Loc("threshold"), 0.72f);
    glUniform1f(s_bloomDown.sh.Loc("knee"), 0.22f);
    GLuint texSrc = texScene;
    int dxSrc = dx, dySrc = dy;
    for (size_t i = 0; i < s_abloom.size(); ++i)
    {
        const LEVEL& l = s_abloom[i];
        glUniform1i(locPrefilter, i == 0 ? 1 : 0);
        glUniform2f(s_bloomDown.rcpSize, 1.0f / float(dxSrc), 1.0f / float(dySrc));
        DrawFullScreen(texSrc, l.fbo, 0, 0, l.dx, l.dy);
        texSrc = l.tex, dxSrc = l.dx, dySrc = l.dy;
    }
    s_bloomUp.sh.Use();
    glUniform1i(s_bloomUp.tex, 0);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE);
    for (size_t i = s_abloom.size() - 1; i > 0; --i)
    {
        const LEVEL& lSmall = s_abloom[i];
        const LEVEL& lLarge = s_abloom[i - 1];
        glUniform2f(s_bloomUp.rcpSize, 1.0f / float(lSmall.dx), 1.0f / float(lSmall.dy));
        DrawFullScreen(lSmall.tex, lLarge.fbo, 0, 0, lLarge.dx, lLarge.dy);
    }
    glDisable(GL_BLEND);
}
}  // namespace

void PostProcessScene(const CAMERA& cam, uint32_t rgbaCel)
{
    if (!kfAvailable || !g_gl.window) return;
    const DISPLAY& d = g_display;
    const bool fAo = d.ambientOcclusion > 0 && s_ao.FReady("ao.frag") && s_aoApply.FReady("aoapply.frag");
    const bool fOutline = d.outlines > 0 && s_outline.FReady("outline.frag");
    const bool fFxaa = d.postAa == 1 && s_fxaa.FReady("fxaa.frag");
    const bool fSharpen = d.upscaler == 1 && d.sharpness > 0 && s_sharpen.FReady("sharpen.frag");
    const bool fBloom = d.bloom > 0 && s_bloomDown.FReady("bloomdown.frag") && s_bloomUp.FReady("bloomup.frag");
    const int grade = d.colorGrade == 2 ? (FEnsureLut() ? 2 : 0) : std::clamp(d.colorGrade, 0, 1);
    const bool fGrade = (fBloom || grade > 0 || d.vignette > 0) && s_grade.FReady("grade.frag");
    const int cpass = int(fAo) + int(fOutline) + int(fFxaa) + int(fSharpen) + int(fGrade);
    if (cpass == 0) return;
    const int dx = g_gl.renderWidth, dy = g_gl.renderHeight;
    EnsureCommon();
    GLSAVE save;
    EnsureTargets(dx, dy);
    // The scene, resolved (MSAA) into the first target: the passes read it and write back into the scene target,
    // where the HUD and the foreground are drawn next.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.FboScene());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_atarget[0].fbo);
    glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (fOutline || fAo)
    {
        EnsureDepth(dx, dy);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.FboScene());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboDepth);
        glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
    // (the glow from the picture as drawn, before the lines and the other passes: the ink does not glow)
    if (fGrade && fBloom) BuildBloom(s_atarget[0].tex, dx, dy);
    // The passes in order, each reading the last one's output; the last writes into the scene target.
    int ipass = 0, itarget = 0;
    auto next = [&]() {
        const bool fLast = ++ipass == cpass;
        const GLuint fbo = fLast ? g_gl.FboScene() : s_atarget[1 - itarget].fbo;
        const GLuint texIn = s_atarget[itarget].tex;
        itarget = 1 - itarget;
        DrawFullScreen(texIn, fbo, 0, 0, dx, dy);
    };
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    const float tanHalfY = std::tan(proj.radFovY * 0.5f);
    if (fAo)
    {
        // the openness into its own target (from the depth alone), then blurred, stepped and applied as a pass
        EnsureAo(dx, dy);
        s_ao.sh.Use();
        glUniform1i(s_ao.sh.Loc("texDepth"), 1);
        glUniform1f(s_ao.sh.Loc("sNear"), cam.sNear);
        glUniform2f(s_ao.sh.Loc("tanHalf"), tanHalfY * float(dx) / float(dy), tanHalfY);
        // (about half Sly's height: a corner's, a crate's, a ledge's shade; the creases inside a character barely)
        glUniform1f(s_ao.sh.Loc("radius"), 70.0f);
        glUniform2f(s_ao.sh.Loc("fade"), 2500.0f * std::max(d.drawDistance, 1.0f), 9000.0f * std::max(d.drawDistance, 1.0f));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_texDepth);
        glBindSampler(1, 0);
        glActiveTexture(GL_TEXTURE0);
        DrawFullScreen(0, s_fboAo, 0, 0, dx, dy);
        s_aoApply.sh.Use();
        glUniform1i(s_aoApply.tex, 0);
        glUniform1i(s_aoApply.sh.Loc("texAo"), 1);
        glUniform1i(s_aoApply.sh.Loc("texDepth"), 2);
        glUniform1f(s_aoApply.sh.Loc("sNear"), cam.sNear);
        glUniform1f(s_aoApply.sh.Loc("strength"), float(std::clamp(d.ambientOcclusion, 0, 100)) / 100.0f);
        // (the characters' mask, unit 3: put back after the pass)
        const GLuint texMask = TexCharacterMaskFrame();
        GLint texUnit3 = 0, samplerUnit3 = 0;
        glActiveTexture(GL_TEXTURE3);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texUnit3);
        glGetIntegerv(GL_SAMPLER_BINDING, &samplerUnit3);
        glBindTexture(GL_TEXTURE_2D, texMask);
        glBindSampler(3, 0);
        glUniform1i(s_aoApply.sh.Loc("texMask"), 3);
        glUniform1i(s_aoApply.sh.Loc("fMask"), texMask ? 1 : 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_texAo);
        glBindSampler(1, 0);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_texDepth);
        glBindSampler(2, 0);
        glActiveTexture(GL_TEXTURE0);
        next();
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, GLuint(texUnit3));
        glBindSampler(3, GLuint(samplerUnit3));
        glActiveTexture(GL_TEXTURE0);
    }
    if (fOutline)
    {
        s_outline.sh.Use();
        glUniform1i(s_outline.tex, 0);
        glUniform1i(s_outline.sh.Loc("texDepth"), 1);
        const float radius = std::max(1.0f, float(dy) / 448.0f);   // (one PS2 pixel, as the original's borders)
        glUniform1f(s_outline.sh.Loc("radius"), radius);
        // the original's border colour (GS units, 0x80 alpha = opaque), a little lighter than solid
        const float a = std::min(float((rgbaCel >> 24) & 0xFF) / 128.0f, 1.0f) * 0.85f;
        glUniform4f(s_outline.sh.Loc("inkRgba"), float(rgbaCel & 0xFF) / 255.0f, float((rgbaCel >> 8) & 0xFF) / 255.0f,
                    float((rgbaCel >> 16) & 0xFF) / 255.0f, a);
        glUniform1f(s_outline.sh.Loc("sNear"), cam.sNear);
        // (about where the original's fog starts thickening, and gone well before its far end)
        glUniform2f(s_outline.sh.Loc("fade"), 3000.0f * std::max(d.drawDistance, 1.0f), 15000.0f * std::max(d.drawDistance, 1.0f));
        glUniform1f(s_outline.sh.Loc("rcpFocal"), 2.0f * tanHalfY / float(dy));
        glUniform1i(s_outline.sh.Loc("fCreases"), d.outlines >= 2 ? 1 : 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_texDepth);
        glBindSampler(1, 0);
        glActiveTexture(GL_TEXTURE0);
        next();
    }
    if (fFxaa)
    {
        s_fxaa.sh.Use();
        glUniform1i(s_fxaa.tex, 0);
        glUniform2f(s_fxaa.rcpSize, 1.0f / float(dx), 1.0f / float(dy));
        next();
    }
    if (fSharpen)
    {
        s_sharpen.sh.Use();
        glUniform1i(s_sharpen.tex, 0);
        glUniform1f(s_sharpen.sharpness, float(std::clamp(d.sharpness, 0, 100)) / 100.0f);
        next();
    }
    if (fGrade)
    {
        s_grade.sh.Use();
        glUniform1i(s_grade.tex, 0);
        glUniform1i(s_grade.sh.Loc("texBloom"), 1);
        glUniform1i(s_grade.sh.Loc("texLut"), 2);
        // (the largest level holds the sum of the chain's levels, each about as bright as the prefiltered picture;
        // 100 % adds it about twice over)
        const float gBloom = fBloom ? float(std::clamp(d.bloom, 0, 100)) / 50.0f / float(s_abloom.size()) : 0.0f;
        glUniform1f(s_grade.sh.Loc("bloom"), gBloom);
        glUniform1i(s_grade.sh.Loc("grade"), grade);
        glUniform1f(s_grade.sh.Loc("lutSize"), float(std::max(s_nLut, 2)));
        glUniform1f(s_grade.sh.Loc("vignette"), float(std::clamp(d.vignette, 0, 100)) / 100.0f);
        glUniform1f(s_grade.sh.Loc("aspect"), float(dx) / float(dy));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, fBloom ? s_abloom[0].tex : 0);
        glBindSampler(1, s_sampler);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_3D, grade == 2 ? s_texLut : 0);
        glBindSampler(2, s_sampler);
        glActiveTexture(GL_TEXTURE0);
        next();
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
    FreeDepth();
    FreeAo();
    FreeBloom();
    if (s_texLut) glDeleteTextures(1, &s_texLut), s_texLut = 0;
    s_stateLut = 0;
    for (POSTPROG* p : {&s_fxaa, &s_sharpen, &s_scale, &s_outline, &s_bloomDown, &s_bloomUp, &s_grade, &s_ao, &s_aoApply})
        if (p->state > 0) p->sh.Delete(), p->state = 0;
    if (s_vao) glDeleteVertexArrays(1, &s_vao), s_vao = 0;
    if (s_sampler) glDeleteSamplers(1, &s_sampler), s_sampler = 0;
}
