// See render.h.

#include "texmod.h"
#include "modelexport.h"
#include "../platform/platform.h"
#include "render.h"
#include "simdmat.h"
#include "drawdist.h"

#include "display.h"
#include "cm.h"
#include "emitter.h"
#include "gl.h"
#include "light.h"
#include "transition.h"
#include "laserrender.h"
#include "pocket.h"
void CollectHackSwarmDraws(const SCENE& scene, std::vector<EXTRADRAW>& aextra);  // (hack.cpp: the swarms' particles)
#include "rtgt.h"
#include "wr.h"
#include "passhelpers.h"

#include <stb/stb_image_write.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string>

RENDERSTATS g_renderstats;
constinit PFNEXTRADRAWS g_pfnExtraDraws = nullptr;
// (PC) the frame's extra draws kept out of the depth outlines (EXTRADRAW fNoOutline: the job markers), drawn into the
// see-through cover at full cover where they are the frame's surface (FBuildTranslucentMask)
static std::vector<EXTRADRAW> s_aextraNoOutline;

struct GLGLOB
{
    GLuint vao = 0;             // the geometry arena's (one per BuildSceneGpu: all its globs' vertices and indices)
    GLuint vbo = 0;             // the arena's vertex buffer (moving geometry: UpdateGlobVerticesGpu)
    GLuint vboColor = 0;        // the arena's lit colours (rgbaA, rgbaB per vertex: attributes 2 / 3; UpdateGlobColorsGpu)
    int32_t baseVertex = 0;     // where this glob's vertices / indices start in the arena
    uint32_t firstIndex = 0;
    GLuint ssboEdges = 0;
    GLuint ssboEdgeSkin = 0;   // per edge vertex: bone weights and indices (skinned cel borders)
    GLuint ssboEdgeNrm = 0;    // (character smoothing) per edge vertex its averaged normal (celborder.vert SMOOTH)
    glm::vec3 posCenter{0.0f};  // the geometry's bounding sphere (for globs without a draw record)
    float sRadius = -1.0f;
    GLsizei cedge = 0;
    GLsizei cindex = 0;
    // its vertices' lowest colour-A alpha as last uploaded: with the texture's lowest, a bound on each fragment's alpha
    // (AllPassOf)
    mutable uint8_t aMinA = 0;
};

// A SUBGLOB's commands, cached: per pass sub-draw its state and order.
struct PASSCMD { uint32_t istate; uint32_t order; };
struct SUBGLOBGPU
{
    const SUBGLOB* powner = nullptr;
    const SHD* pshd = nullptr;
    int iframe = -1;
    GLuint vao = 0;
    uint8_t fCull = 0;
    std::vector<PASSCMD> apasscmd;
    SUBGLOBGPU* pnextVao = nullptr;   // the same SUBGLOB drawn from another arena (an instance's own vertices)
    uint32_t genTex = 0;              // the texture cache's generation its states were made in (s_genTexCache)
};

namespace
{
void InitCommonOnce();
struct GPUVTX
{
    float pos[3];
    float uv[2];
    uint8_t rgbaA[4];
    uint8_t rgbaB[4];
    float normal[3];
    uint8_t abone[4];   // skinning (animation code): the group's bone indices (VTXGLOB::abone)
    uint8_t aw[4];      // and the raw weights (/255, P_G 726)
};
static_assert(sizeof(GPUVTX) == 48);
// (the colours' own buffer: GPUVTX's rgbaA / rgbaB are not read from the vertex buffer)
struct GPUCOL
{
    uint8_t rgbaA[4];
    uint8_t rgbaB[4];
};
static_assert(sizeof(GPUCOL) == 8);
inline GPUCOL ColOfGpuVtx(const GPUVTX& v)
{
    GPUCOL c;
    std::memcpy(c.rgbaA, v.rgbaA, 4);
    std::memcpy(c.rgbaB, v.rgbaB, 4);
    return c;
}

std::vector<std::unique_ptr<GLGLOB>> s_aglglob;
std::map<std::pair<const BMP*, const CLUT*>, GLuint> s_mptex;
// (bumped when the texture cache is reset: a SUBGLOB's cached states name its GL textures, which are deleted then. Only
// the PC guests' kept-world globs (pcloader.h) outlive a world load with their cache: they are drawn again in the next
// world and must not keep the deleted names, which the new world's textures reuse: R5x, the guests' wrong textures
// after a death's reload)
uint32_t s_genTexCache = 1;
GLuint s_texWhite = 0;
GLuint s_asampler[16] = {};  // bit 0 clamp S, 1 clamp T, 2 nearest, 3 mipmapped (a texture mod's own mip chain: texmod.h)
// The mipmapped samplers' anisotropy (texture mods only; SLY2N_MODANISO=N, 1: off): clamped to the GPU's maximum
const float s_rModAniso = std::getenv("SLY2N_MODANISO") ? float(std::atof(std::getenv("SLY2N_MODANISO"))) : 8.0f;
// (image quality, display.h anisotropy above 1: the original textures get a mip chain too, made at upload, and the
// mipmapped samplers this anisotropy. 1: the original's single level, as before.)
std::unordered_set<GLuint> s_settexMip;
inline bool FMipOriginals()
{
#if defined(__SWITCH__) || defined(__APPLE__)
    return false;
#else
    return g_display.anisotropy > 1;
#endif
}
// The sampler for a texture: the mipmapped one for a texture mod or a mipmapped original
inline uint8_t SamplerFor(GLuint tex, uint8_t sampler)
{
    return uint8_t(sampler | (FTextureIsMod(tex) || (!s_settexMip.empty() && s_settexMip.count(tex)) ? 8 : 0));
}
GLuint s_vaoEmpty = 0;
// Geometry arenas: the globs built by one BuildSceneGpu share one VBO / EBO / VAO, so a run of draws with one GL
// state is one glMultiDrawElementsIndirect. Attribute 6 is the draw index (an iota buffer, divisor 1, selected by
// the command's baseInstance) into the per-frame draw data (glob.vert DRAWS / RECS).
std::vector<struct GPUVTX> s_avtxArena;
std::vector<uint32_t> s_aindexArena;
std::vector<GLGLOB*> s_apglArena;
// (TEST, character smoothing: gl.h g_cSmoothGlob) per arena vertex the averaged normal glob.geom bends along (attribute
// 8, a buffer of its own; 0: the vertex is not smoothed). Only kept while the smoothing is on.
std::vector<glm::vec3> s_anrmArena;
GLuint s_vboDrawIndex = 0;
constexpr uint32_t CDRAW_MAX = 1u << 18;

// (the old world's textures: deleted at the next lookup, where GL is current)
std::vector<GLuint> s_atexFreed;

// Per GL texture of TexFromSel its lowest texel alpha (0: not known, a texture mod's): a filtered sample, a weighted
// mean of texels, is never below it. Per pass state AllPassOf's answer, cleared when a texture name changes meaning.
std::unordered_map<GLuint, uint8_t> s_mptexMinA;
struct ALLPASS
{
    float vNeed = std::numeric_limits<float>::infinity();   // the lowest vertex alpha x object alpha that passes always
    uint32_t istateAlt = UINT32_MAX;                         // the draw's state then, without the test (none: not drawn)
    bool fKnown = false;
};
std::vector<ALLPASS> s_aallpass;
void NoteTexMinAlpha(GLuint tex, uint8_t aMin)
{
    s_mptexMinA[tex] = aMin;
    s_aallpass.clear();
}

// (perf-3, g_fGlobBindless) the resident handles of (texture, sampler) pairs the pass draws use; a deleted texture's are
// made non-resident first and forgotten (its name may come back), and the per-state cache is invalidated
std::unordered_map<uint64_t, GLuint64> s_mphandle;
uint32_t s_genHandle = 1;
GLuint64 TexHandle(GLuint tex, uint8_t sampler)
{
    const uint64_t key = (uint64_t(tex) << 8) | (sampler & 15u);
    auto [it, fNew] = s_mphandle.try_emplace(key, 0);
    if (fNew)
    {
        it->second = glGetTextureSamplerHandleARB(tex, s_asampler[sampler & 15u]);
        if (it->second) glMakeTextureHandleResidentARB(it->second);
    }
    return it->second;
}
void ForgetTexHandles(const std::vector<GLuint>& atex)
{
    if (s_mphandle.empty()) return;
    for (GLuint tex : atex)
        for (uint64_t i = 0; i < 16; ++i)
        {
            const auto it = s_mphandle.find((uint64_t(tex) << 8) | i);
            if (it == s_mphandle.end()) continue;
            if (it->second) glMakeTextureHandleNonResidentARB(it->second);
            s_mphandle.erase(it);
        }
    ++s_genHandle;
}

GLuint TexFromSel(const TEXSEL& sel)
{
    InitCommonOnce();
    if (!s_atexFreed.empty())
    {
        ForgetTexHandles(s_atexFreed);
        glDeleteTextures(GLsizei(s_atexFreed.size()), s_atexFreed.data());
        s_atexFreed.clear();
    }
    if (!sel.pbmp) return s_texWhite;
    const std::pair<const BMP*, const CLUT*> key(sel.pbmp, sel.pclut);
    auto it = s_mptex.find(key);
    if (it != s_mptex.end()) return it->second;
    std::vector<uint8_t> rgba;
    std::string error;
    GLuint tex = s_texWhite;
    if (FDecodeTexture(sel.pbmp, sel.pclut, rgba, error))
    {
        // (a texture mod: Mods/Textures/<W>x<H>_<hash>.png)
        if (const GLuint texMod = GlTexFromMod(rgba, sel.pbmp->dx, sel.pbmp->dy))
        {
            s_mptex[key] = texMod;
            NoteTexMinAlpha(texMod, 0);   // (the replacement's own alpha: not known here)
            ++g_renderstats.ctexture;
            return texMod;
        }
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sel.pbmp->dx, sel.pbmp->dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        if (FMipOriginals() && (sel.pbmp->dx > 1 || sel.pbmp->dy > 1))
        {
            // (a mip texel is a mean of the level above: never below the texture's lowest alpha, so AllPassOf holds)
            glGenerateMipmap(GL_TEXTURE_2D);
            s_settexMip.insert(tex);
        }
        uint8_t aMin = 255;
        for (size_t i = 3; i < rgba.size(); i += 4) aMin = std::min(aMin, rgba[i]);
        NoteTexMinAlpha(tex, rgba.size() >= 4 ? aMin : 0);
        ++g_renderstats.ctexture;
    }
    else
    {
        ++g_renderstats.cfailTexture;
        static int s_cwarn = 0;
        if (s_cwarn++ < 10) std::fprintf(stderr, "texture 0x%X: %s\n", sel.pbmp->id, error.c_str());
    }
    s_mptex[key] = tex;
    return tex;
}

void InitCommon();
void InitCommonOnce() { InitCommon(); }
void InitCommon()
{
    if (s_texWhite) return;
    const uint32_t white = 0x80FFFFFFu;  // RGB 255, alpha 0x80 (1.0)
    glGenTextures(1, &s_texWhite);
    glBindTexture(GL_TEXTURE_2D, s_texWhite);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    NoteTexMinAlpha(s_texWhite, 0x80);
    glGenSamplers(16, s_asampler);
    constexpr GLenum kMaxAniso = 0x84FF, kAniso = 0x84FE;  // GL_(MAX_)TEXTURE_MAX_ANISOTROPY (4.6, EXT before)
    GLfloat rAnisoMax = 0.0f;
    glGetFloatv(kMaxAniso, &rAnisoMax);
    while (glGetError() != GL_NO_ERROR) {}
    const float rAniso = std::min(FMipOriginals() ? float(g_display.anisotropy) : s_rModAniso, rAnisoMax);
    for (int i = 0; i < 16; ++i)
    {
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_WRAP_S, (i & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_WRAP_T, (i & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        const GLint filter = (i & 4) ? GL_NEAREST : GL_LINEAR;
        // (mipmapped, for a texture mod: the original's filter between texels, trilinear / nearest-mip between sizes;
        // the anisotropy only where the original is bilinear, so point-sampled pixel art stays crisp)
        const GLint filterMin = !(i & 8) ? filter : (i & 4) ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_MIN_FILTER, filterMin);
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_MAG_FILTER, filter);
        if ((i & 8) && !(i & 4) && rAniso > 1.0f) glSamplerParameterf(s_asampler[i], kAniso, rAniso);
    }
    glGenVertexArrays(1, &s_vaoEmpty);
    std::vector<uint32_t> aiota(CDRAW_MAX);
    for (uint32_t i = 0; i < CDRAW_MAX; ++i) aiota[i] = i;
    glGenBuffers(1, &s_vboDrawIndex);
    glBindBuffer(GL_ARRAY_BUFFER, s_vboDrawIndex);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(aiota.size() * 4), aiota.data(), GL_STATIC_DRAW);
}

template <class F> void SetBounds(GLGLOB* pgl, size_t c, F posOf)
{
    if (c == 0) return;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (size_t i = 0; i < c; ++i) { const glm::vec3 p = posOf(i); lo = glm::min(lo, p); hi = glm::max(hi, p); }
    pgl->posCenter = (lo + hi) * 0.5f;
    float r2 = 0;
    for (size_t i = 0; i < c; ++i) { const glm::vec3 d = posOf(i) - pgl->posCenter; r2 = std::max(r2, glm::dot(d, d)); }
    pgl->sRadius = std::sqrt(r2);
}

void AppendArena(GLGLOB* pgl, const std::vector<GPUVTX>& av, const std::vector<uint32_t>& ai,
                 const std::vector<glm::vec3>* panrmSmooth = nullptr)
{
    if (g_cSmoothGlob > 0)
    {
        if (panrmSmooth && panrmSmooth->size() == av.size()) s_anrmArena.insert(s_anrmArena.end(), panrmSmooth->begin(), panrmSmooth->end());
        else s_anrmArena.resize(s_anrmArena.size() + av.size(), glm::vec3(0.0f));
    }
    SetBounds(pgl, av.size(), [&](size_t i) { return glm::vec3(av[i].pos[0], av[i].pos[1], av[i].pos[2]); });
    pgl->baseVertex = int32_t(s_avtxArena.size());
    pgl->firstIndex = uint32_t(s_aindexArena.size());
    uint8_t aMin = 255;
    for (const GPUVTX& v : av) aMin = std::min(aMin, v.rgbaA[3]);
    pgl->aMinA = av.empty() ? 0 : aMin;
    s_avtxArena.insert(s_avtxArena.end(), av.begin(), av.end());
    s_aindexArena.insert(s_aindexArena.end(), ai.begin(), ai.end());
    s_apglArena.push_back(pgl);
}

void FlushArena()
{
    if (s_apglArena.empty()) return;
    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(s_avtxArena.size() * sizeof(GPUVTX)), s_avtxArena.data(), GL_STATIC_DRAW);
    // (perf-3) the colours in a buffer of their own: the run-time lighting rewrites them (8 bytes a vertex, not the
    // whole vertex), a dynamic buffer (Mesa warns on, and may stall for, updates of a static one)
    GLuint vboColor = 0;
    {
        std::vector<GPUCOL> acol(s_avtxArena.size());
        for (size_t i = 0; i < acol.size(); ++i) acol[i] = ColOfGpuVtx(s_avtxArena[i]);
        glGenBuffers(1, &vboColor);
        glBindBuffer(GL_ARRAY_BUFFER, vboColor);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(acol.size() * sizeof(GPUCOL)), acol.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
    }
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(s_aindexArena.size() * 4), s_aindexArena.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, uv)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, normal)));
    glEnableVertexAttribArray(5);
    glVertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, abone)));
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, aw)));
    glBindBuffer(GL_ARRAY_BUFFER, vboColor);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GPUCOL), reinterpret_cast<void*>(offsetof(GPUCOL, rgbaA)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GPUCOL), reinterpret_cast<void*>(offsetof(GPUCOL, rgbaB)));
    glBindBuffer(GL_ARRAY_BUFFER, s_vboDrawIndex);
    glEnableVertexAttribArray(6);
    glVertexAttribIPointer(6, 1, GL_UNSIGNED_INT, 4, nullptr);
    glVertexAttribDivisor(6, 1);
    if (g_cSmoothGlob > 0 && s_anrmArena.size() == s_avtxArena.size())
    {
        // (character smoothing) attribute 8: the averaged normals (a buffer of their own, never updated)
        GLuint vboNrm = 0;
        glGenBuffers(1, &vboNrm);
        glBindBuffer(GL_ARRAY_BUFFER, vboNrm);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(s_anrmArena.size() * sizeof(glm::vec3)), s_anrmArena.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(8);
        glVertexAttribPointer(8, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    }
    std::vector<glm::vec3>().swap(s_anrmArena);
    glBindVertexArray(0);
    for (GLGLOB* pgl : s_apglArena) { pgl->vao = vao; pgl->vbo = vbo; pgl->vboColor = vboColor; }
    s_apglArena.clear();
    std::vector<GPUVTX>().swap(s_avtxArena);
    std::vector<uint32_t>().swap(s_aindexArena);
}

void GpuVerticesOfGlob(const GLOB& glob, std::vector<GPUVTX>& av)
{
    av.resize(glob.avtx.size());
    for (size_t i = 0; i < av.size(); ++i)
    {
        const VTXGLOB& v = glob.avtx[i];
        std::memcpy(av[i].pos, &v.pos, 12);
        std::memcpy(av[i].uv, &v.uv, 8);
        std::memcpy(av[i].rgbaA, &v.rgbaA, 4);
        std::memcpy(av[i].rgbaB, &v.rgbaB, 4);
        std::memcpy(av[i].normal, &v.normal, 12);
        std::memcpy(av[i].abone, v.abone, 4);  // (skinning, animation code)
        std::memcpy(av[i].aw, v.aw, 4);
    }
}

void BuildGlobGpu(GLOB& glob)
{
    if (glob.pgl) return;  // (a GLOBSET shared by several scene entries)
    if (glob.meshk == MESHK_Strips && !glob.avtx.empty() && !glob.aindex.empty())
    {
        auto pgl = std::make_unique<GLGLOB>();
        std::vector<GPUVTX> av;
        GpuVerticesOfGlob(glob, av);
        AppendArena(pgl.get(), av, glob.aindex, &glob.anrmSmooth);
        glob.pgl = pgl.get();
        s_aglglob.push_back(std::move(pgl));
    }
    else if (glob.meshk == MESHK_Indexed && !glob.asubfx.empty())
    {
        // Type 3 (shadow hulls of rp 7 / 8): per strip the position dictionary indexed per strip vertex, triangles
        // where the strip mask's (ADC) bit is clear, winding alternating with the vertex index as in type 0.
        std::vector<GPUVTX> av;
        std::vector<uint32_t> ai;
        for (const SUBFX& fx : glob.asubfx)
        {
            const uint32_t base = uint32_t(av.size());
            for (size_t k = 0; k < fx.aipos.size(); ++k)
            {
                GPUVTX v{};
                v.rgbaA[3] = 128;
                const glm::vec3 p = fx.aipos[k] < fx.apos.size() ? fx.apos[fx.aipos[k]] : glm::vec3(0.0f);
                std::memcpy(v.pos, &p, 12);
                av.push_back(v);
                const bool fAdc = (fx.amask[k / 32] >> (k % 32)) & 1;
                if (k < 2 || fAdc) continue;
                const uint32_t i = base + uint32_t(k);
                if (k & 1) { ai.push_back(i - 1); ai.push_back(i - 2); }
                else { ai.push_back(i - 2); ai.push_back(i - 1); }
                ai.push_back(i);
            }
        }
        if (ai.empty()) return;
        auto pgl = std::make_unique<GLGLOB>();
        AppendArena(pgl.get(), av, ai);
        pgl->cindex = GLsizei(ai.size());
        glob.pgl = pgl.get();
        s_aglglob.push_back(std::move(pgl));
    }
    else if (glob.meshk == MESHK_CelBorder && !glob.asubcel.empty())
    {
        std::vector<glm::vec4> aedge;
        struct EDGESKIN { glm::vec4 w; glm::uvec4 ab; };   // per edge vertex: weights and the group's bone indices
        std::vector<EDGESKIN> askin;
        std::vector<glm::vec4> anrm;   // (character smoothing) per edge vertex its averaged normal (w 0: none)
        bool fNrm = false;
        for (const SUBCEL& cel : glob.asubcel)
            for (size_t i = 0; i + 3 < cel.aedge.size(); i += 4)
                for (int j = 0; j < 4; ++j)
                {
                    // The stream's records are {OA, E0, E1, OB} (Sly 1's TWEF order 1, 2, 0, 3): checked on Paris, the
                    // triangles (OA, E0, E1) and (E1, E0, OB) of every record share their winding.
                    static const int s_aj[4] = {1, 2, 0, 3};
                    const int jSrc = s_aj[j];
                    const uint8_t ipos = cel.aedge[i + size_t(jSrc)];
                    aedge.emplace_back(ipos < cel.apos.size() ? cel.apos[ipos] : glm::vec3(0.0f), 1.0f);
                    EDGESKIN sk{glm::vec4(0.0f), glm::uvec4(cel.aiboneSlot[0], cel.aiboneSlot[1], cel.aiboneSlot[2], cel.aiboneSlot[3])};
                    for (int k = 0; k < cel.cw && k < 4; ++k)
                        if (size_t(ipos) * size_t(cel.cw) + size_t(k) < cel.aw.size())
                            sk.w[k] = float(cel.aw[size_t(ipos) * size_t(cel.cw) + size_t(k)]) / 255.0f;
                    askin.push_back(sk);
                    glm::vec3 n(0.0f);
                    if (ipos < cel.anrmSmooth.size()) n = cel.anrmSmooth[ipos];
                    fNrm = fNrm || n != glm::vec3(0.0f);
                    anrm.emplace_back(n, n != glm::vec3(0.0f) ? 1.0f : 0.0f);
                }
        if (aedge.empty()) return;
        auto pgl = std::make_unique<GLGLOB>();
        glGenBuffers(1, &pgl->ssboEdges);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, pgl->ssboEdges);
        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(aedge.size() * sizeof(glm::vec4)), aedge.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &pgl->ssboEdgeSkin);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, pgl->ssboEdgeSkin);
        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(askin.size() * sizeof(EDGESKIN)), askin.data(), GL_STATIC_DRAW);
        if (g_fSmoothCel && fNrm)
        {
            glGenBuffers(1, &pgl->ssboEdgeNrm);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, pgl->ssboEdgeNrm);
            glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(anrm.size() * sizeof(glm::vec4)), anrm.data(), GL_STATIC_DRAW);
        }
        pgl->cedge = GLsizei(aedge.size() / 4);
        SetBounds(pgl.get(), aedge.size(), [&](size_t i) { return glm::vec3(aedge[i]); });
        glob.pgl = pgl.get();
        s_aglglob.push_back(std::move(pgl));
    }
}

// GS ALPHA -> GL blend: Cv = (A - B) * C + D, A/B/D in {Cs, Cd, 0}, C in {As, Ad, FIX}. Each colour's coefficient
// is k1 + kC * C; GL factors cover 0, 1, C and 1 - C (and -C via reverse subtract).
bool FSetBlend(uint64_t alpha)
{
    const int a = int(alpha & 3), b = int((alpha >> 2) & 3), c = int((alpha >> 4) & 3), d = int((alpha >> 6) & 3);
    const float fix = float((alpha >> 32) & 0xFF) / 128.0f;
    auto coef = [&](int sel, int& k1, int& kc) {
        k1 = (d == sel) ? 1 : 0;
        kc = (a == sel ? 1 : 0) - (b == sel ? 1 : 0);
    };
    int s1, sc, d1, dc;
    coef(0, s1, sc);
    coef(1, d1, dc);
    const GLenum cf = c == 0 ? GL_SRC_ALPHA : c == 1 ? GL_DST_ALPHA : GL_CONSTANT_ALPHA;
    const GLenum cfInv = c == 0 ? GL_ONE_MINUS_SRC_ALPHA : c == 1 ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_CONSTANT_ALPHA;
    if (c == 2) glBlendColor(0, 0, 0, fix);
    auto factor = [&](int k1, int kc, GLenum& f) {
        if (k1 == 0 && kc == 0) f = GL_ZERO;
        else if (k1 == 1 && kc == 0) f = GL_ONE;
        else if (k1 == 0 && kc == 1) f = cf;
        else if (k1 == 1 && kc == -1) f = cfInv;
        else return false;
        return true;
    };
    // The GS blends only the colour: the frame's alpha is the source alpha (the shadow pass, 0x16DA48 / 0x16DD58,
    // reads it: render.md "shadow frame-channel pass").
    GLenum fs, fd;
    if (sc == -1 && s1 == 0 && factor(d1, dc, fd))
    {
        glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
        glBlendFuncSeparate(cf, fd, GL_ONE, GL_ZERO);
        return true;
    }
    glBlendEquation(GL_FUNC_ADD);
    if (!factor(s1, sc, fs) || !factor(d1, dc, fd))
    {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        return false;
    }
    glBlendFuncSeparate(fs, fd, GL_ONE, GL_ZERO);
    return true;
}

struct DRAWREC
{
    int kind;
    float z;
    const GLOB* pglob;      // the GLOB (for its shaders / visibility)
    const GLOB* pglobGeom;  // where its geometry lives
    const glm::mat4* pmat;
    const std::vector<glm::mat4>* pamatBone = nullptr;  // skinned: the bone matrices (they replace pmat)
    float alpha = 1.0f;                                 // the object alpha (qw8.x; RIPs' fades)
    float alpha2 = 1.0f;                                // the cel borders' (qw8.y)
    const SCENEGLOBSET* psgs = nullptr;                 // the entry and the group (cast shadows)
    size_t iglob = 0;
    uint32_t iboneFirst = 0;                            // their place in the frame's bone SSBO
};

#ifdef __SWITCH__
// (the Switch) What RenderPass's command loop reads of the next records, prefetched in stages along their chains of
// pointers: the groups six records ahead, their entries four ahead, what those point to two ahead, the passes one.
void PrefetchRecordsAhead(const std::vector<DRAWREC>& arec, size_t irec)
{
    constexpr size_t CSUB = 4;   // (the entries looked at per group)
    const size_t crec = arec.size();
    if (irec + 6 < crec)
    {
        const DRAWREC& r = arec[irec + 6];
        __builtin_prefetch(r.pglob);
        __builtin_prefetch(r.pglobGeom);
        __builtin_prefetch(&r.pglobGeom->iSpec);
        __builtin_prefetch(&r.pglobGeom->asubglob);
        __builtin_prefetch(&r.pglobGeom->pgl);
        __builtin_prefetch(r.pmat);
        if (r.psgs) __builtin_prefetch(&r.psgs->apglInst);
    }
    if (irec + 4 < crec)
    {
        const DRAWREC& r = arec[irec + 4];
        const std::vector<SUBGLOB>& asub = r.pglobGeom->asubglob;
        for (size_t k = 0; k < asub.size() && k < CSUB; ++k) __builtin_prefetch(&asub[k]);
        if (r.psgs && r.iglob < r.psgs->apglInst.size()) __builtin_prefetch(&r.psgs->apglInst[r.iglob]);
        __builtin_prefetch(r.pglobGeom->pgl);
    }
    if (irec + 2 < crec)
    {
        const DRAWREC& r = arec[irec + 2];
        const std::vector<SUBGLOB>& asub = r.pglobGeom->asubglob;
        for (size_t k = 0; k < asub.size() && k < CSUB; ++k)
        {
            __builtin_prefetch(asub[k].pshd);
            __builtin_prefetch(asub[k].pgpu);
            if (asub[k].psaa) __builtin_prefetch(&asub[k].psaa->iframe);
        }
        if (r.psgs && r.iglob < r.psgs->apglInst.size()) __builtin_prefetch(r.psgs->apglInst[r.iglob]);
    }
    if (irec + 1 < crec)
    {
        const std::vector<SUBGLOB>& asub = arec[irec + 1].pglobGeom->asubglob;
        for (size_t k = 0; k < asub.size() && k < CSUB; ++k)
            if (const SUBGLOBGPU* p = asub[k].pgpu) __builtin_prefetch(p->apasscmd.data());
    }
}
#endif

struct UNIFORMS
{
    GLint matWorldToClip, icolor, rgbaConst, posCamera, dirCamera;
    GLint tex, fTextured, ate, atst, aref, afailMode, fogColor, fFogBlack, fDate, texDate;
};
// The glob programs: 0 glGlobShader, 1 glGlobShaderNoKill (no discards: the runs without alpha test and DATE). Each has
// its own uniform locations and values; s_pu is the bound one's (RenderPass's useGlob).
UNIFORMS s_au[2];
const UNIFORMS* s_pu = &s_au[0];
int s_iprogGlob = 0;
GLSHADER* const s_apshGlob[2] = {&glGlobShader, &glGlobShaderNoKill};
// The glob uniforms outside DSTATE that a draw inherits from whatever set them last (rgbaConst: the volumes and hulls;
// fFogBlack: the passes): kept per program, brought to the last value set when it is bound (SyncGlobInherited).
struct GLOBINHERIT { glm::vec4 rgbaConst{0.0f}; int fFogBlack = 0; };
GLOBINHERIT s_aglobinh[2], s_globinhLast;
void SetGlobRgbaConst(float r, float g, float b, float a)
{
    glUniform4f(s_pu->rgbaConst, r, g, b, a);
    s_aglobinh[s_iprogGlob].rgbaConst = s_globinhLast.rgbaConst = glm::vec4(r, g, b, a);
}
struct CELUNIFORMS { GLint matWorldToClip, matModel, halfWidthNdc, edgeBase, celRgba, cbone, iboneFirst, csegSmooth; } s_uc;

glm::vec4 Vec4FromRgba(uint32_t rgba, float scale)
{
    return glm::vec4(float(rgba & 0xFF), float((rgba >> 8) & 0xFF), float((rgba >> 16) & 0xFF), float(rgba >> 24)) * scale;
}

// Per-frame draw data (glob.vert): per record its matrix and fog, per draw (record x entry) the SAA's ST offset,
// the object alpha and the record.
// bone: first matrix, count (skinned), the vertices' base in the arena; spec0 / spec1 / specPos: GLOB::iSpec's view-angle
// alpha (P_G 638): {g0, g1, g2, kind}, {clamp lo, hi, on, 0}, {posSpec, 0}; warp: the state vectors' first + 1 (0: no
// warp), the glob's vertex count, the first matrix, the matrix count | 0x100 positions | 0x200 coordinates (wr.h)
struct RECGPU { glm::mat4 model; glm::vec4 fog; glm::uvec4 bone{0u}; glm::vec4 spec0{0.0f}, spec1{0.0f}, specPos{0.0f}; glm::uvec4 warp{0u}; };
struct DRAWGPU { float du, dv, uAlpha; uint32_t irec; };

// The GL state a run of draws shares (one glMultiDrawElementsIndirect): everything DrawPass used to set per pass.
struct DSTATE
{
    GLuint vao = 0, tex = 0;
    uint64_t alpha = 0;         // GS ALPHA (fBlend)
    uint8_t sampler = 0, fTextured = 0, icolor = 0, fBlend = 0;
    uint8_t fCull = 0, depthFunc = 0, depthMask = 0, colorMask = 0;   // depthFunc 0 always 1 gequal 2 greater; colorMask 1 rgb 2 a
    uint8_t ate = 0, atst = 0, aref = 0, afailMode = 0;
    uint8_t fFogBlack = 0;      // FOGCOL 0 (0x1EB960: the additive passes of kinds 1 / 7)
    uint8_t fFba = 0;           // FBA 1 (kinds 4 / 6: 0x16E3C8's GS block 0x2DF700): the alpha's bit 7 (stencil STENCIL_FBA)
    uint8_t pad[2] = {};
};
static_assert(sizeof(DSTATE) == 32);
bool operator==(const DSTATE& a, const DSTATE& b) { return std::memcmp(&a, &b, sizeof(DSTATE)) == 0; }

enum CMDK : uint8_t { CMDK_Pass, CMDK_Volume, CMDK_Shadow, CMDK_Cel, CMDK_Particles, CMDK_Shadow2, CMDK_ShadowBegin, CMDK_ShadowKind8, CMDK_ShadowCombine, CMDK_FbaClear, CMDK_ShadowMap };
// Interned states: a command refers to its state by index.
struct DSTATEHASH
{
    size_t operator()(const DSTATE& st) const
    {
        uint64_t h = 1469598103934665603ull;
        const uint8_t* pb = reinterpret_cast<const uint8_t*>(&st);
        for (size_t i = 0; i < sizeof(DSTATE); ++i) h = (h ^ pb[i]) * 1099511628211ull;
        return size_t(h);
    }
};
std::vector<DSTATE> s_astate;
std::unordered_map<DSTATE, uint32_t, DSTATEHASH> s_mpstate;
uint32_t IstateOf(const DSTATE& st)
{
    auto it = s_mpstate.find(st);
    if (it != s_mpstate.end()) return it->second;
    const uint32_t i = uint32_t(s_astate.size());
    s_astate.push_back(st);
    s_mpstate.emplace(st, i);
    return i;
}

// The state with FBA set (render kinds 4 / 6).
std::vector<uint32_t> s_aistateFba;
uint32_t IstateFba(uint32_t istate)
{
    if (istate >= s_aistateFba.size()) s_aistateFba.resize(istate + 1, UINT32_MAX);
    if (s_aistateFba[istate] == UINT32_MAX)
    {
        DSTATE st = s_astate[istate];
        st.fFba = 1;
        const uint32_t i = IstateOf(st);
        if (istate >= s_aistateFba.size()) s_aistateFba.resize(istate + 1, UINT32_MAX);
        s_aistateFba[istate] = i;
    }
    return s_aistateFba[istate];
}

// A pass state's alpha test decided by the draw's alpha bounds: the GS alpha floor(t x v / 128) (glob.frag) is at least
// tMin x vMin / 128, so a GEQUAL / GREATER test that bound reaches always passes (with half a unit for the filtering).
ALLPASS AllPassOf(uint32_t istate)
{
    if (istate < s_aallpass.size() && s_aallpass[istate].fKnown) return s_aallpass[istate];
    ALLPASS ap;
    ap.fKnown = true;
    const DSTATE st = s_astate[istate];
    const int need = st.atst == 5 ? int(st.aref) : st.atst == 6 ? int(st.aref) + 1 : 256;
    if (st.ate && (st.afailMode == 1 || st.afailMode == 2) && st.icolor == 0 && need <= 255)
    {
        uint8_t tMin = 0x80;
        if (st.fTextured)
        {
            const auto it = s_mptexMinA.find(st.tex);
            tMin = it != s_mptexMinA.end() ? it->second : 0;
        }
        if (tMin > 0)
        {
            ap.vNeed = 128.0f * (float(need) + 0.5f) / float(tMin);
            if (st.afailMode == 1)
            {
                DSTATE stAlt = st;
                stAlt.ate = stAlt.atst = stAlt.aref = stAlt.afailMode = 0;
                ap.istateAlt = IstateOf(stAlt);
            }
        }
    }
    if (istate >= s_aallpass.size()) s_aallpass.resize(size_t(istate) + 1);
    s_aallpass[istate] = ap;
    return ap;
}

std::deque<SUBGLOBGPU> s_asubgpu;

struct CMD
{
    uint32_t istate = 0;
    GLuint vao = 0;             // (specials)
    uint32_t order = 0;         // pass x 4 + its alpha-test sub-draw (the order within a record)
    uint32_t count = 0, firstIndex = 0;
    int32_t baseVertex = 0;
    uint32_t idraw = 0;
    CMDK cmdk = CMDK_Pass;
    const DRAWREC* prec = nullptr;
    const SUBGLOB* psub = nullptr;
};
struct DEICMD { uint32_t count, instanceCount, firstIndex; int32_t baseVertex; uint32_t baseInstance; };

GLuint s_ssboRec = 0, s_ssboDraw = 0, s_bufIndirect = 0, s_ssboBone = 0, s_ssboWarp = 0, s_ssboWarpMat = 0, s_ssboTexh = 0;

// (perf-3) The passes' per-frame draw data (records, draw entries, bones, warps, the indirect commands, the texture
// handles) written in place into one persistently mapped buffer (GL 4.4 glBufferStorage, coherent): no copy through the
// driver's staging memory and its thread. Three segments, one a frame (RenderScene), each reused after its fence (three
// frames on). A pass that does not fit the frame's segment uploads as before (orphan + sub-data), and the ring doubles
// at the next frame. Opt-in, SLY2N_PERSIST=1: on Mesa (radeonsi, threaded) it measured the same as the orphaned
// buffers, and some drivers place persistent coherent memory in system memory, which the vertex stage reads per vertex.
struct STREAMRING
{
    static constexpr int CSEG = 3;
    GLuint buf = 0;
    uint8_t* pb = nullptr;
    size_t cbSeg = 0, off = 0, offLim = 0, align = 256;
    int iseg = 0;
    GLsync async[CSEG] = {};
    bool fGrow = false, fDisabled = false;
    typedef void (*PFNBUFFERSTORAGE)(GLenum, GLsizeiptr, const void*, GLbitfield);
    PFNBUFFERSTORAGE pfnBufferStorage = nullptr;

    void Create(size_t cb)
    {
        constexpr GLbitfield grf = 0x0002 /*MAP_WRITE*/ | 0x0040 /*MAP_PERSISTENT*/ | 0x0080 /*MAP_COHERENT*/;
        glGenBuffers(1, &buf);
        glBindBuffer(GL_COPY_WRITE_BUFFER, buf);
        pfnBufferStorage(GL_COPY_WRITE_BUFFER, GLsizeiptr(cb * CSEG), nullptr, grf);
        pb = static_cast<uint8_t*>(glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, GLsizeiptr(cb * CSEG), grf));
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        if (!pb)
        {
            glDeleteBuffers(1, &buf);
            buf = 0;
            fDisabled = true;
            return;
        }
        cbSeg = cb;
    }
    void Destroy()
    {
        for (GLsync& sync : async)
            if (sync) { glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull); glDeleteSync(sync); sync = nullptr; }
        if (buf)
        {
            glBindBuffer(GL_COPY_WRITE_BUFFER, buf);
            glUnmapBuffer(GL_COPY_WRITE_BUFFER);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            glDeleteBuffers(1, &buf);
        }
        buf = 0;
        pb = nullptr;
    }
    // The start of a frame: the last segment fenced, the next one waited for.
    void BeginFrame()
    {
        if (fDisabled) return;
        if (!pfnBufferStorage)
        {
            static const bool s_fOff = std::getenv("SLY2N_PERSIST") == nullptr;
            pfnBufferStorage = reinterpret_cast<PFNBUFFERSTORAGE>(cane::gl::ProcAddress("glBufferStorage"));
#if defined(__linux__)
            // (a 4.3 context, Zink on ARM GPUs: Mesa returns an entry point even when the driver lacks the feature)
            if ((GLVersion.major < 4 || (GLVersion.major == 4 && GLVersion.minor < 4)) && !glfwExtensionSupported("GL_ARB_buffer_storage")) pfnBufferStorage = nullptr;
#endif
            if (s_fOff || !pfnBufferStorage) { fDisabled = true; return; }
            GLint a = 0;
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &a);
            align = std::max<size_t>(256, size_t(std::max(a, 1)));
        }
        if (buf)
        {
            if (async[iseg]) glDeleteSync(async[iseg]);
            async[iseg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
        if (fGrow || !buf)
        {
            const size_t cb = buf ? cbSeg * 2 : size_t(4) << 20;
            Destroy();
            fGrow = false;
            Create(cb);
            if (!buf) return;
        }
        iseg = (iseg + 1) % CSEG;
        if (async[iseg])
        {
            // (polled first: a waiting call with the flush makes a threaded driver catch up first)
            if (glClientWaitSync(async[iseg], 0, 0) == GL_TIMEOUT_EXPIRED)
                glClientWaitSync(async[iseg], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            glDeleteSync(async[iseg]);
            async[iseg] = nullptr;
        }
        off = cbSeg * size_t(iseg);
        offLim = off + cbSeg;
    }
    // cb bytes (aligned) of this frame's segment: their offset, or SIZE_MAX when they do not fit
    size_t Alloc(size_t cb)
    {
        if (!pb) return SIZE_MAX;
        const size_t o = (off + align - 1) / align * align;
        if (o + cb > offLim) { fGrow = true; return SIZE_MAX; }
        off = o + cb;
        return o;
    }
};
STREAMRING s_ring;

// (g_fGlobBindless) a state's texture handle (0: untextured, or none made: then drawn with the texture bound) and its
// merge class (the state without its texture and sampler, interned: equal classes differ only in those)
struct HANDLEOFSTATE { uint32_t gen = 0; uint32_t iclass = UINT32_MAX; GLuint64 handle = 0; };
std::vector<HANDLEOFSTATE> s_ahandleIstate;
std::unordered_map<DSTATE, uint32_t, DSTATEHASH> s_mpclass;
const HANDLEOFSTATE& HandleEntryOfIstate(uint32_t istate)
{
    if (istate >= s_ahandleIstate.size()) s_ahandleIstate.resize(istate + 1);
    HANDLEOFSTATE& e = s_ahandleIstate[istate];
    if (e.gen != s_genHandle)
    {
        DSTATE st = s_astate[istate];
        e.gen = s_genHandle;
        e.handle = st.fTextured ? TexHandle(st.tex, st.sampler) : 0;
        st.tex = 0;
        st.sampler = 0;
        e.iclass = s_mpclass.try_emplace(st, uint32_t(s_mpclass.size())).first->second;
    }
    return e;
}
GLuint64 HandleOfIstate(uint32_t istate) { return HandleEntryOfIstate(istate).handle; }
// Two pass states one run can draw with handles: the same but for the texture and sampler (both textured, with handles)
bool FSameButTexture(uint32_t istateA, uint32_t istateB)
{
    if (istateA == istateB) return true;
    HandleEntryOfIstate(istateA);
    HandleEntryOfIstate(istateB);   // (both made first: a lookup may grow the vector)
    const HANDLEOFSTATE& a = s_ahandleIstate[istateA];
    const HANDLEOFSTATE& b = s_ahandleIstate[istateB];
    return a.iclass == b.iclass && a.handle && b.handle;
}

// (SLY2N_GLSTATS=N: the GL state calls per frame, averaged over N frames: the draw path's costs on the driver)
struct GLSTATS { long capply, cfull, ctex, csampler, cuniform, cblend, crun, cpass; };
GLSTATS s_glstats{};
const int s_cframeGlStats = std::getenv("SLY2N_GLSTATS") ? std::atoi(std::getenv("SLY2N_GLSTATS")) : 0;

// Applies a run's state, setting only what changed since the last run (fForce: everything).
struct STATECACHE
{
    DSTATE cur;
    bool fValid = false;
    // the uniforms each glob program was last given (a program keeps its own), and the bound program
    DSTATE aucur[2];
    bool afuValid[2] = {};
    int iprog = 0;
    // everything set outside Apply: the GL state and both programs' uniforms set again at the next run
    void Invalidate() { fValid = false; afuValid[0] = afuValid[1] = false; }
    // fBindTexture false: the run's draws take their textures from handles (g_fGlobBindless): the unit's binding is left
    // as it is (and forgotten: the next run that binds one binds it)
    void Apply(const DSTATE& st, bool fBindTexture = true)
    {
        const bool f = !fValid;
        const bool fu = !afuValid[iprog];   // (the bound program's uniforms)
        const DSTATE& ucur = aucur[iprog];
        if (s_cframeGlStats)
        {
            ++s_glstats.capply; s_glstats.cfull += f;
            
            s_glstats.ctex += fBindTexture && st.fTextured && (f || !cur.fTextured || st.tex != cur.tex || !fTexBound);
            s_glstats.csampler += fBindTexture && st.fTextured && (f || !cur.fTextured || st.sampler != cur.sampler || !fTexBound);
            s_glstats.cuniform += (fu || st.fTextured != ucur.fTextured) + (fu || st.icolor != ucur.icolor) + (fu || st.ate != ucur.ate) + (fu || st.atst != ucur.atst) +
                                  (fu || st.aref != ucur.aref) + (fu || st.afailMode != ucur.afailMode) + (fu || st.fFogBlack != ucur.fFogBlack);
            s_glstats.cblend += st.fBlend && (f || !cur.fBlend || st.alpha != cur.alpha);
        }
        if (f || st.vao != cur.vao) glBindVertexArray(st.vao);
        if (fBindTexture && st.fTextured && (f || !cur.fTextured || st.tex != cur.tex || !fTexBound)) glBindTexture(GL_TEXTURE_2D, st.tex);
        if (fBindTexture && st.fTextured && (f || !cur.fTextured || st.sampler != cur.sampler || !fTexBound)) glBindSampler(0, s_asampler[st.sampler]);
        if (fu || st.fTextured != ucur.fTextured) glUniform1i(s_pu->fTextured, st.fTextured);
        if (fu || st.icolor != ucur.icolor) glUniform1i(s_pu->icolor, st.icolor);
        if (f || st.fBlend != cur.fBlend) { if (st.fBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
        if (st.fBlend && (f || !cur.fBlend || st.alpha != cur.alpha)) FSetBlend(st.alpha);
        if (f || st.fCull != cur.fCull)
        {
            if (st.fCull) { glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); }
            else glDisable(GL_CULL_FACE);
        }
        if (f) glEnable(GL_DEPTH_TEST);
        if (f || st.depthFunc != cur.depthFunc) glDepthFunc(st.depthFunc == 0 ? GL_ALWAYS : st.depthFunc == 1 ? GL_GEQUAL : GL_GREATER);
        if (f || st.depthMask != cur.depthMask) glDepthMask(st.depthMask ? GL_TRUE : GL_FALSE);
        if (f || st.colorMask != cur.colorMask)
        {
            const GLboolean c = (st.colorMask & 1) ? GL_TRUE : GL_FALSE;
            glColorMask(c, c, c, (st.colorMask & 2) ? GL_TRUE : GL_FALSE);
        }
        if (fu || st.ate != ucur.ate) glUniform1i(s_pu->ate, st.ate);
        if (fu || st.atst != ucur.atst) glUniform1i(s_pu->atst, st.atst);
        if (fu || st.aref != ucur.aref) glUniform1f(s_pu->aref, float(st.aref));
        if (fu || st.afailMode != ucur.afailMode) glUniform1i(s_pu->afailMode, st.afailMode);
        if (fu || st.fFogBlack != ucur.fFogBlack) glUniform1i(s_pu->fFogBlack, st.fFogBlack);
        s_aglobinh[iprog].fFogBlack = s_globinhLast.fFogBlack = st.fFogBlack;
        // the frame alpha's bit 7 (FBA) in the stencil: set by the FBA kinds' alpha writes, cleared by the others'
        // (the native frame keeps alpha / 128, clamped: the shadow pass's DATE needs the bit, its CLUT the bits below)
        if (f || st.fFba != cur.fFba || st.colorMask != cur.colorMask)
        {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, st.fFba ? 0x80 : 0, 0x80);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glStencilMask((st.colorMask & 2) ? 0x80 : 0);
        }
        cur = st;
        fValid = true;
        aucur[iprog] = st;
        afuValid[iprog] = true;
        fTexBound = fBindTexture;
    }
    bool fTexBound = true;
};
// a run's state draws with the program without discards (glGlobShaderNoKill) unless it alpha-tests
inline bool FStateNeedsKill(const DSTATE& st) { return st.ate && st.afailMode; }

// The bound glob program's inherited uniforms brought to the last values set (GLOBINHERIT); a changed fFogBlack makes
// sc set that program's uniforms again at its next run.
void SyncGlobInherited(STATECACHE& sc)
{
    GLOBINHERIT& h = s_aglobinh[s_iprogGlob];
    if (h.rgbaConst != s_globinhLast.rgbaConst)
    {
        glUniform4fv(s_pu->rgbaConst, 1, &s_globinhLast.rgbaConst[0]);
        h.rgbaConst = s_globinhLast.rgbaConst;
    }
    if (h.fFogBlack != s_globinhLast.fFogBlack)
    {
        glUniform1i(s_pu->fFogBlack, s_globinhLast.fFogBlack);
        h.fFogBlack = s_globinhLast.fFogBlack;
        sc.afuValid[s_iprogGlob] = false;
    }
}

// One shader pass of an entry (formerly drawn directly): the GL state its GS registers ask for, as 1..2 commands
// (the alpha test's pass / fail sub-draws).
void EmitPass(std::vector<PASSCMD>& apasscmd, DSTATE st, const SUBGLOB& sub, const SHDPASS& pass, int ipass)
{
    // Texture
    if (pass.fTextured)
    {
        const TEXSEL sel = TexselFromShd(sub.pshd, sub.psaa ? sub.psaa->iframe : 0, pass.itexPass);
        st.tex = TexFromSel(sel);
        st.sampler = SamplerFor(st.tex, uint8_t((sel.fClampS ? 1 : 0) | (sel.fClampT ? 2 : 0) | (pass.fNearest ? 4 : 0)));
    }
    st.fTextured = pass.fTextured ? 1 : 0;
    st.icolor = uint8_t(pass.icolor);
    st.fFogBlack = pass.fFogBlack ? 1 : 0;
    // Blend (PRIM ABE)
    st.fBlend = pass.fBlend ? 1 : 0;
    st.alpha = pass.fBlend ? pass.alpha : 0;
    const bool fWritesColor = !((pass.alpha & 0xFF) == 0x40 || ((pass.alpha & 0xFF) == 0x50));  // Cd: (X - X) * C + Cd
    // Depth test (TEST ZTE / ZTST; reversed Z)
    const uint64_t test = pass.test;
    const bool zte = (test >> 16) & 1;
    const int ztst = int((test >> 17) & 3);
    if (zte && ztst == 0) return;
    st.depthFunc = uint8_t(!zte || ztst == 1 ? 0 : ztst == 2 ? 1 : 2);
    // Alpha test (TEST ATE / ATST / AREF / AFAIL)
    const bool ate = test & 1;
    const int atst = int((test >> 1) & 7);
    const int aref = int((test >> 4) & 0xFF);
    const int afail = int((test >> 12) & 3);
    st.ate = ate ? 1 : 0;
    st.atst = uint8_t(atst);
    st.aref = uint8_t(aref);
    auto draw = [&](int iSub, int afailMode, bool fDepth, bool fColor, bool fAlpha) {
        DSTATE stDraw = st;
        stDraw.afailMode = uint8_t(afailMode);
        stDraw.depthMask = fDepth ? 1 : 0;
        stDraw.colorMask = uint8_t((fColor ? 1 : 0) | (fColor && fAlpha ? 2 : 0));
        apasscmd.push_back({IstateOf(stDraw), uint32_t(ipass * 4 + iSub)});
    };
    // The tests the fragment's alpha (0..255 after the GS's clamp, glob.frag) always passes or always fails, by kind
    // and reference, drawn without the test (no discards): the AFAIL sub-draw alone for one that always fails.
    const bool fPassAll = atst == 1 || (atst == 5 && aref == 0) || (atst == 3 && aref >= 255);
    const bool fFailAll = atst == 0 || (atst == 2 && aref == 0) || (atst == 6 && aref >= 255);
    if (ate && (fPassAll || fFailAll)) st.ate = st.atst = st.aref = 0;   // (as without the test: one state, longer runs)
    if (!ate || fPassAll)
        draw(0, 0, true, fWritesColor, true);
    else if (fFailAll)
    {
        switch (afail)
        {
        case 1: draw(1, 0, false, fWritesColor, true); break;    // FB_ONLY
        case 2: draw(1, 0, true, false, false); break;           // ZB_ONLY
        case 3: draw(1, 0, false, fWritesColor, false); break;   // RGB_ONLY
        default: break;                                          // KEEP: nothing drawn
        }
    }
    else
    {
        if (atst != 0) draw(0, 1, true, fWritesColor, true);
        switch (afail)
        {
        case 1: draw(1, 2, false, fWritesColor, true); break;    // FB_ONLY
        case 2: draw(1, 2, true, false, false); break;           // ZB_ONLY
        case 3: draw(1, 2, false, fWritesColor, false); break;   // RGB_ONLY
        default: break;                                          // KEEP
        }
    }
}

// The entry's cached commands (rebuilt when its shader or SAA frame changes).
const SUBGLOBGPU& SubgpuOf(const SUBGLOB& sub, GLuint vao, uint8_t fCull)
{
    SUBGLOBGPU* p = sub.pgpu;
    if (!p || p->powner != &sub)
    {
        p = &s_asubgpu.emplace_back();
        p->powner = &sub;
        sub.pgpu = p;
    }
    // (one cache per arena: the instances' own vertex copies live in other arenas)
    while (p->vao != vao && p->vao != 0)
    {
        if (!p->pnextVao)
        {
            SUBGLOBGPU* pnew = &s_asubgpu.emplace_back();
            pnew->powner = &sub;
            p->pnextVao = pnew;
        }
        p = p->pnextVao;
    }
    const int iframe = sub.psaa ? sub.psaa->iframe : 0;
    if (p->pshd != sub.pshd || p->iframe != iframe || p->vao != vao || p->fCull != fCull || p->genTex != s_genTexCache)
    {
        p->genTex = s_genTexCache;
        p->pshd = sub.pshd;
        p->iframe = iframe;
        p->vao = vao;
        p->fCull = fCull;
        p->apasscmd.clear();
        DSTATE st;
        st.vao = vao;
        st.fCull = fCull;
        for (size_t ipass = 0; ipass < sub.pshd->apass.size(); ++ipass)
            EmitPass(p->apasscmd, st, sub, sub.pshd->apass[ipass], int(ipass));
    }
    return *p;
}

// ------------------------------------------------------------ cast shadows (mesh type 2)
// What its VU1 program draws, reconstructed from the data and 0x1A4478's shadow quadwords (ShadowOfLightContext):
// the mesh's records are edges {OA, E0, E1, OB} (vertex index x 2: each vertex twice in VU memory, as itself and
// projected), so the program extrudes the silhouette (the edges between a face toward the shadow's centre and one
// away from it) from the vertices to their projections from the centre onto the far plane (centre + axis, normal
// along the axis), with the faces toward the centre as the near cap and their projections as the far cap: a
// shadow volume. The pixels inside are darkened like the type-3 hulls (z-fail stencil, the shader's alpha).
std::vector<glm::vec3> s_avtxShadow2;
#ifdef __SWITCH__
std::vector<uint32_t> s_acvtxShadow2Caster;   // (the Switch: the vertex count of each of the pass's casters, in order)
#endif
GLSHADER s_shShadowVol, s_shShadowVolNoKill;
GLuint s_vaoShadowVol = 0, s_vboShadowVol = 0;

#if !defined(__SWITCH__) && !defined(__APPLE__)
// (PC character shadow maps) a type-2 caster's shadow hull vertices in world space, as FBuildShadow2 skins them,
// appended to av
void SkinShadowHulls(const SCENEGLOBSET& sgs, size_t iglob, const GLOB& glob, std::vector<glm::vec3>& av)
{
    if (iglob >= sgs.amatGlob.size()) return;
    const glm::mat4& mat = sgs.amatGlob[iglob];
    const std::vector<glm::mat4>* pamatBone = iglob < sgs.aamatBone.size() && !sgs.aamatBone[iglob].empty() ? &sgs.aamatBone[iglob] : nullptr;
    for (const SUBFX& fx : glob.asubfx)
        for (size_t v = 0; v < fx.apos.size(); ++v)
        {
            const glm::vec4 p(fx.apos[v], 1.0f);
            glm::vec3 w(0.0f);
            if (pamatBone && fx.cw > 0)
            {
                for (int j = 0; j < fx.cw && j < 4; ++j)
                {
                    const size_t ib = std::min<size_t>(fx.aiboneSlot[j], pamatBone->size() - 1);
                    w += float(fx.aw[v * size_t(fx.cw) + size_t(j)]) / 255.0f * glm::vec3(MulMat4Vec4((*pamatBone)[ib], p));
                }
            }
            else w = glm::vec3(MulMat4Vec4(mat, p));
            av.push_back(w);
        }
}
#endif

// (the volume's vertices appended to avtxOut, cmd's range in it; any thread: the scratch is the thread's own)
bool FBuildShadow2(const SCENEGLOBSET& sgs, size_t iglob, const GLOB& glob, const glm::vec3& posCamera, float sNear,
                   std::vector<glm::vec3>& avtxOut, CMD& cmd)
{
    static const bool s_fDbg = std::getenv("SLY2_SH2DBG") != nullptr;
    static const char* const s_szOnly = std::getenv("SLY2_SH2ONLY");
    if (s_fDbg)
        std::printf("shadow2: oid %d sgs %p glob %zu sMRD %g grfGroup 0x%X n0 %d cbone %d fx %zu oidOwner %d gZ %g\n", sgs.oid, (const void*)&sgs, iglob, glob.sMRD,
                    glob.grfGroup, glob.n0, glob.cbone, glob.asubfx.size(), glob.oidOwner, glob.gZOrder);
    if (s_szOnly && int(iglob) != std::atoi(s_szOnly)) return false;
    const glm::mat4& mat = sgs.amatGlob[iglob];
    const std::vector<glm::mat4>* pamatBone = iglob < sgs.aamatBone.size() && !sgs.aamatBone[iglob].empty() ? &sgs.aamatBone[iglob] : nullptr;
    // the world positions (skinned by the weights' bones)
    static thread_local std::vector<std::vector<glm::vec3>> s_aav;
#ifdef __SWITCH__
    // (the Switch: never shrunk, so the pieces' vectors keep their storage from one caster to the next)
    if (s_aav.size() < glob.asubfx.size()) s_aav.resize(glob.asubfx.size());
#else
    s_aav.resize(glob.asubfx.size());
#endif
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (size_t k = 0; k < glob.asubfx.size(); ++k)
    {
        const SUBFX& fx = glob.asubfx[k];
        auto& av = s_aav[k];
        av.resize(fx.apos.size());
        for (size_t v = 0; v < fx.apos.size(); ++v)
        {
            const glm::vec4 p(fx.apos[v], 1.0f);
            glm::vec3 w(0.0f);
            if (pamatBone && fx.cw > 0)
            {
                for (int j = 0; j < fx.cw && j < 4; ++j)
                {
                    const size_t ib = std::min<size_t>(fx.aiboneSlot[j], pamatBone->size() - 1);
                    w += float(fx.aw[v * size_t(fx.cw) + size_t(j)]) / 255.0f * glm::vec3(MulMat4Vec4((*pamatBone)[ib], p));
                }
            }
            else w = glm::vec3(MulMat4Vec4(mat, p));
            av[v] = w;
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
    }
    const glm::vec3 posCtx = sgs.fPosLight ? sgs.posLight : (lo + hi) * 0.5f;
    float sRadius = 0;   // (pl +0x1C: the shadow-casting hierarchy's radius about the context's position)
    for (size_t k = 0; k < glob.asubfx.size(); ++k)
        for (const glm::vec3& v : s_aav[k]) sRadius = std::max(sRadius, glm::length(v - posCtx));
    const SHADOWCTX sh = ShadowOfLightContext(WorldLights(), posCtx, sRadius, 400.0f, posCamera, sNear);   // (+0x13C 0x2C3B90)
    if (!sh.fValid) return false;
    if (s_fDbg)
        std::printf("shadow2: lo %.0f %.0f %.0f hi %.0f %.0f %.0f bones %zu\n", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, pamatBone ? pamatBone->size() : 0),
        std::printf("shadow2: ctx %.0f %.0f %.0f r %.0f centre %.0f %.0f %.0f axis %.0f %.0f %.0f\n", posCtx.x, posCtx.y, posCtx.z, sRadius,
                    sh.center.x, sh.center.y, sh.center.z, sh.axis.x, sh.axis.y, sh.axis.z);
    const glm::vec3 dir = glm::normalize(sh.axis);
    const glm::vec3 posFar = sh.center + sh.axis;
    const float dFar = glm::dot(posFar - sh.center, dir);
    // PC QoL "Long shadows" (display.h DISPLAY::fLongShadows, default on; NOT the original, render.md "Long shadows"):
    // the original ends every prism at the plane 400 past the caster along the axis (+0x13C), so a low light (the
    // title's window) cuts the shadow off at a straight edge on the floor (R39). Here a ray from the centre through a
    // vertex that descends is lengthened, never shortened, until it reaches the horizontal plane kLongShadowDepth below
    // the caster's lowest vertex (its feet: the floor it stands on), at most kLongShadowMaxExtra past the vertex. So the
    // far end is under the floor the shadow falls on (out of view) instead of on it, and the extension cannot reach
    // more than kLongShadowDepth below the caster (no shadows through floors onto the level below, no streaks across
    // the level). Rays that do not descend (a light below the vertex) keep the original's length.
    constexpr float kLongShadowDepth = 50.0f, kLongShadowMaxExtra = 2000.0f;
    static const bool s_fLongOff = [] { const char* sz = std::getenv("SLY2N_LONGSHADOWS"); return sz && sz[0] == '0'; }();
    const bool fLong = g_display.fLongShadows && !s_fLongOff;
    const float zLongFloor = lo.z - kLongShadowDepth;
    auto project = [&](const glm::vec3& v) {
        const glm::vec3 dvc = v - sh.center;
        const float dv = glm::dot(dvc, dir);
        if (dv <= 1.0f) return v + dir * dFar;
        float s = dFar / dv;
        const float dzDown = -dvc.z;   // (how far the vertex is below the centre)
        if (fLong && dzDown > 1e-3f)
        {
            const float sFloor = (sh.center.z - zLongFloor) / dzDown;
            if (sFloor > s) s = std::min(sFloor, std::max(s, 1.0f + kLongShadowMaxExtra / glm::length(dvc)));
        }
        return sh.center + dvc * s;
    };
    const uint32_t ivtxFirst = uint32_t(avtxOut.size());
    // (perf-3: a prism's 24 vertices written through one resize, not 24 push_backs)
    glm::vec3* pout = nullptr;
    auto tri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, const glm::vec3& outward) {
        if (glm::dot(glm::cross(b - a, c - a), outward) < 0.0f) std::swap(b, c);
        pout[0] = a;
        pout[1] = b;
        pout[2] = c;
        pout += 3;
    };
    for (size_t k = 0; k < glob.asubfx.size(); ++k)
    {
        const SUBFX& fx = glob.asubfx[k];
        const auto& av = s_aav[k];
        auto P = [&](uint8_t u) { const size_t i = u / 2; return i < av.size() ? av[i] : glm::vec3(0.0f); };
        auto lit = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
            return glm::dot(glm::cross(b - a, c - a), dir) < 0.0f;
        };
        // each face once (it is in three edge records), as its own closed prism from the face to its projection:
        // the union of the prisms is the shadow whatever the mesh's closure and winding (the records' edges
        // between faces toward and away from the centre are the silhouette of a closed mesh; the hulls are not
        // all closed: a silhouette-only volume leaks there)
        // (each face once: a bit per sorted vertex triple; the records index at most 128 vertices x 2)
        static thread_local std::vector<uint64_t> s_abDone((size_t(1) << 21) / 64, 0);
        static thread_local std::vector<uint32_t> s_akDone;
        for (uint32_t k : s_akDone) s_abDone[k >> 6] = 0;
        s_akDone.clear();
#ifdef __SWITCH__
        // (the Switch: the same prisms without the side walls two of them share from opposite sides: the same four
        // corners, one counting +1 where the other counts -1 in z-pass and z-fail alike, so the pair counts nothing.
        // Only a hairline along the wall's diagonal, which each prism split its own way, can count differently.)
        struct PRISM { glm::vec3 av[6]; glm::vec3 m; uint8_t grfWall; };
        static thread_local std::vector<PRISM> s_aprism;
        static thread_local std::vector<uint64_t> s_awall;   // (edge << 32 | prism << 3 | wall << 1 | facing)
        s_aprism.clear();
        s_awall.clear();
        auto prism = [&](uint8_t ia, uint8_t ib, uint8_t ic) {
            std::array<uint8_t, 3> key{ia, ib, ic};
            if (key[1] < key[0]) std::swap(key[0], key[1]);
            if (key[2] < key[1]) std::swap(key[1], key[2]);
            if (key[1] < key[0]) std::swap(key[0], key[1]);
            if (key[0] == key[1] || key[1] == key[2]) return;
            const uint32_t k = (uint32_t(key[0] >> 1) << 14) | (uint32_t(key[1] >> 1) << 7) | uint32_t(key[2] >> 1);
            if (s_abDone[k >> 6] & (uint64_t(1) << (k & 63))) return;
            s_abDone[k >> 6] |= uint64_t(1) << (k & 63);
            s_akDone.push_back(k);
            const glm::vec3 a = P(ia), b = P(ib), c = P(ic);
            if (!lit(a, b, c) && !lit(a, c, b)) return;   // (edge-on)
            PRISM p{{a, b, c, project(a), project(b), project(c)}, glm::vec3(0.0f), 7};
            p.m = (a + b + c + p.av[3] + p.av[4] + p.av[5]) / 6.0f;
            const uint32_t iprism = uint32_t(s_aprism.size());
            const uint8_t ai[3] = {uint8_t(ia >> 1), uint8_t(ib >> 1), uint8_t(ic >> 1)};
            for (uint32_t w = 0; w < 3; ++w)
            {
                // the edge (w, w + 1)'s wall from its lower vertex, and the prism's side of it. Only a flat wall pairs
                // (a bent one, a corner from project's axis branch, differs by diagonal); an unclear facing is kept.
                const uint32_t i0 = ai[w] < ai[(w + 1) % 3] ? w : (w + 1) % 3, i1 = i0 == w ? (w + 1) % 3 : w;
                const glm::vec3 &wa = p.av[i0], &wb = p.av[i1], &waf = p.av[3 + i0], &wbf = p.av[3 + i1];
                const glm::vec3 n = glm::cross(wb - wa, waf - wa);
                const float ln = glm::length(n);
                if (!(ln > 0.0f) || std::fabs(glm::dot(n, wbf - wa)) > 1e-3f * ln * glm::length(wbf - wa)) continue;
                const glm::vec3 out = (wa + wb + waf + wbf) * 0.25f - p.m;
                const float d = glm::dot(n, out);
                if (!(std::fabs(d) > 1e-4f * ln * glm::length(out))) continue;
                const uint32_t edge = uint32_t(ai[i0]) << 8 | ai[i1];
                s_awall.push_back(uint64_t(edge) << 32 | uint64_t(iprism) << 3 | uint64_t(w) << 1 | (d > 0.0f ? 1u : 0u));
            }
            s_aprism.push_back(p);
        };
        for (size_t r = 0; r + 3 < fx.arec.size(); r += 4)
        {
            const uint8_t oa = fx.arec[r], e0 = fx.arec[r + 1], e1 = fx.arec[r + 2], ob = fx.arec[r + 3];
            prism(oa, e0, e1);
            prism(e1, e0, ob);
        }
        // the walls left out: an edge's exactly two, of opposite facings
        std::sort(s_awall.begin(), s_awall.end());
        for (size_t i = 0; i < s_awall.size();)
        {
            size_t j = i + 1;
            while (j < s_awall.size() && (s_awall[j] >> 32) == (s_awall[i] >> 32)) ++j;
            if (j - i == 2 && ((s_awall[i] ^ s_awall[i + 1]) & 1))
                for (size_t q = i; q < j; ++q) s_aprism[size_t((s_awall[q] >> 3) & 0x1FFFFFFF)].grfWall &= uint8_t(~(1u << ((s_awall[q] >> 1) & 3)));
            i = j;
        }
        size_t cvtxPiece = 0;
        for (const PRISM& p : s_aprism) cvtxPiece += 6 + 6 * size_t(std::popcount(unsigned(p.grfWall)));
        const size_t ivtxPiece = avtxOut.size();
        avtxOut.resize(ivtxPiece + cvtxPiece);
        pout = avtxOut.data() + ivtxPiece;
        for (const PRISM& p : s_aprism)
        {
            const glm::vec3 &a = p.av[0], &b = p.av[1], &c = p.av[2], &af = p.av[3], &bf = p.av[4], &cf = p.av[5];
            auto side = [&](const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2) { tri(v0, v1, v2, (v0 + v1 + v2) / 3.0f - p.m); };
            side(a, b, c);
            side(af, bf, cf);
            if (p.grfWall & 1) { side(a, b, bf); side(a, bf, af); }
            if (p.grfWall & 2) { side(b, c, cf); side(b, cf, bf); }
            if (p.grfWall & 4) { side(c, a, af); side(c, af, cf); }
        }
#else
        auto prism = [&](uint8_t ia, uint8_t ib, uint8_t ic) {
            std::array<uint8_t, 3> key{ia, ib, ic};
            // (sorted: a three-element network, the same order as std::sort)
            if (key[1] < key[0]) std::swap(key[0], key[1]);
            if (key[2] < key[1]) std::swap(key[1], key[2]);
            if (key[1] < key[0]) std::swap(key[0], key[1]);
            if (key[0] == key[1] || key[1] == key[2]) return;
            const uint32_t k = (uint32_t(key[0] >> 1) << 14) | (uint32_t(key[1] >> 1) << 7) | uint32_t(key[2] >> 1);
            if (s_abDone[k >> 6] & (uint64_t(1) << (k & 63))) return;
            s_abDone[k >> 6] |= uint64_t(1) << (k & 63);
            s_akDone.push_back(k);
            const glm::vec3 a = P(ia), b = P(ib), c = P(ic);
            if (!lit(a, b, c) && !lit(a, c, b)) return;   // (edge-on)
            const glm::vec3 af = project(a), bf = project(b), cf = project(c);
            const glm::vec3 m = (a + b + c + af + bf + cf) / 6.0f;
            const size_t ivtx = avtxOut.size();
            avtxOut.resize(ivtx + 24);
            pout = avtxOut.data() + ivtx;
            auto side = [&](const glm::vec3& p, const glm::vec3& q, const glm::vec3& r) { tri(p, q, r, (p + q + r) / 3.0f - m); };
            side(a, b, c);
            side(af, bf, cf);
            side(a, b, bf); side(a, bf, af);
            side(b, c, cf); side(b, cf, bf);
            side(c, a, af); side(c, af, cf);
        };
        for (size_t r = 0; r + 3 < fx.arec.size(); r += 4)
        {
            const uint8_t oa = fx.arec[r], e0 = fx.arec[r + 1], e1 = fx.arec[r + 2], ob = fx.arec[r + 3];
            prism(oa, e0, e1);
            prism(e1, e0, ob);
        }
#endif
    }
    cmd.firstIndex = ivtxFirst;
    cmd.count = uint32_t(avtxOut.size()) - ivtxFirst;
    cmd.idraw = uint32_t(glob.asubfx[0].pshd->argba[0] >> 24);   // (the darkening alpha)
    return cmd.count > 0;
}

// The stencil while a shadow phase is open (0x16DA48 .. 0x16DD58), as the original's frame green: kind 7 counts in
// bits 0..6 from 0x3F (0x16DA48's fill 0x7E, the hulls' +-2 per face: G bit 7), so bit 6 is set where the count is
// 1..64; kind 8 counts in bits 0..5 from 0x1F (fill 0x3E keeping bit 7: G bit 6), bit 5 set at 1..32, bit 6 kept.
// STENCIL_FBA: the frame alpha's bit 7 the FBA kinds wrote (STATECACHE).
constexpr GLuint STENCIL_FBA = 0x80, STENCIL_SH7 = 0x40, STENCIL_SH8 = 0x20, STENCIL_COUNT7 = 0x7F, STENCIL_COUNT8 = 0x3F;

// fDateMarked: the stencil's STENCIL_FBA holds the phase's DATE (FMarkDate), so the program without the discard draws.
// pplaneNear (the Switch): the plane past which a vertex lies in front of the drawn near plane (n . p + d > 0).
void DrawShadow2(const CMD& cmd, const glm::mat4& matWorldToClip, bool fDateMarked, const glm::vec4* pplaneNear = nullptr)
{
    if (!s_shShadowVol.ID)
    {
        s_shShadowVol.Init("shadowvol.vert", "shadowvol.frag");
        // the variant without the DATE discard (ID 0 if it does not build: the one above draws)
        if (!s_shShadowVolNoKill.FInit("shadowvol.vert", "shadowvol.frag", "#define NOKILL 1\n")) s_shShadowVolNoKill.ID = 0;
        glGenVertexArrays(1, &s_vaoShadowVol);
        glGenBuffers(1, &s_vboShadowVol);
        glBindVertexArray(s_vaoShadowVol);
        glBindBuffer(GL_ARRAY_BUFFER, s_vboShadowVol);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    }
    const bool fDate = !(cmd.order & 8);   // (the shader's flag 8: no DATE)
    const GLSHADER& sh = (!fDate || fDateMarked) && s_shShadowVolNoKill.ID ? s_shShadowVolNoKill : s_shShadowVol;
    sh.Use();
    glUniformMatrix4fv(sh.Loc("matWorldToClip"), 1, GL_FALSE, glm::value_ptr(matWorldToClip));
    glUniform1i(sh.Loc("texDate"), 1);
    glUniform1i(sh.Loc("fDate"), fDate && !fDateMarked ? 1 : 0);
    glUniform4f(sh.Loc("rgba"), 0, 0, 0, 0);
    glBindVertexArray(s_vaoShadowVol);
    glBindBuffer(GL_ARRAY_BUFFER, s_vboShadowVol);
    const glm::vec3* pvtx = s_avtxShadow2.data() + cmd.firstIndex;
    uint32_t cpass = 0;   // (the Switch: the vertices counted z-pass, uploaded first)
#ifdef __SWITCH__
    // (the Switch: a caster's volume wholly in front of the drawn near plane neither holds the camera nor is cut by it,
    // so z-pass counts it as z-fail does, and its fragments behind the scene (most of a long shadow) fail early Z)
    static std::vector<glm::vec3> s_avtxSplit;
    uint32_t cvtxCasters = 0;
    for (uint32_t c : s_acvtxShadow2Caster) cvtxCasters += c;
    if (pplaneNear && cvtxCasters == cmd.count)
    {
        s_avtxSplit.resize(cmd.count);
        const glm::vec3 n(*pplaneNear);
        uint32_t cfail = cmd.count, i = 0;
        for (const uint32_t c : s_acvtxShadow2Caster)
        {
            bool fFront = true;
            for (uint32_t k = 0; k < c && fFront; ++k) fFront = glm::dot(n, pvtx[i + k]) + pplaneNear->w > 0.0f;
            if (fFront) { std::copy(pvtx + i, pvtx + i + c, s_avtxSplit.data() + cpass); cpass += c; }
            else { cfail -= c; std::copy(pvtx + i, pvtx + i + c, s_avtxSplit.data() + cfail); }
            i += c;
        }
        pvtx = s_avtxSplit.data();
    }
#endif
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(size_t(cmd.count) * sizeof(glm::vec3)), pvtx, GL_STREAM_DRAW);
    // the prisms' z-fail count into kind 7's (closed volumes: the original's +-2 per face in front of the scene)
    glEnable(GL_STENCIL_TEST);
    glStencilMask(STENCIL_COUNT7);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_EQUAL, 0, (cmd.order & 8) ? 0 : STENCIL_FBA);   // (DATE: the alpha's bit 7 clear)
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
    if (cpass)
    {
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(cpass));
    }
    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
    if (cmd.count > cpass) glDrawArrays(GL_TRIANGLES, GLint(cpass), GLsizei(cmd.count - cpass));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
}

void DrawOne(const CMD& cmd)
{
    glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, GLsizei(cmd.count), GL_UNSIGNED_INT,
                                                  reinterpret_cast<void*>(size_t(cmd.firstIndex) * 4), 1, cmd.baseVertex, cmd.idraw);
    ++g_renderstats.cpass;
}

// Shader kind 3 (rp 11 / 13): a projected volume. 0x1EB960's four lists use the destination alpha's bit 7 as a
// mask (two untextured passes over the volume, a DATE-tested fill with (rgba1 + 1) / 2, a last pass with rgba2);
// as in Sly1-Release's DrawProjVolume the mask is the stencil's bit 7: set where the scene is in front of the
// volume's back faces, cleared where it is in front of its front faces, then filled where it stays set.
void DrawProjVolume(const CMD& cmd)
{
    const SUBGLOB& sub = *cmd.psub;
    const SHD* pshd = sub.pshd;
    const uint8_t fl = pshd->grfshd;
    // (debug: SLY2N_VOLPRINT=1: each volume drawn, its object, shader and place)
    if (static const bool s_fDbg = std::getenv("SLY2N_VOLPRINT") != nullptr; s_fDbg)
        std::printf("vol: oid %d glob %zu kind %d shd %d grfshd 0x%X tris %u rgba %08X %08X %08X at %.0f %.0f %.0f\n",
                    cmd.prec->psgs ? cmd.prec->psgs->oid : -1, cmd.prec->iglob, cmd.prec->kind, pshd->id, unsigned(fl), cmd.count / 3,
                    pshd->argba[0], pshd->argba[1], pshd->argba[2], (*cmd.prec->pmat)[3].x, (*cmd.prec->pmat)[3].y, (*cmd.prec->pmat)[3].z);
    glBindVertexArray(cmd.vao);
    glUniform1i(s_pu->fTextured, 0);
    glUniform1i(s_pu->ate, 0);
    glUniform1i(s_pu->afailMode, 0);
    glm::vec4 c;
    for (int i = 0; i < 4; ++i) c[i] = float(((pshd->argba[1] >> (8 * i)) & 0xFF) + 1) / 2.0f;
    if (!(fl & 2))
    {
        // An untextured volume (the murk of rp 13, e.g. the intro's rooftop 150055): the mask as the original's lists
        // make it, so only the scene inside the volume is filled (the gang's silhouettes in the dark), not the
        // volume's faces in front of everything: bit 7 set where the scene is in front of a back face (z-fail),
        // cleared where it is in front of a front face, filled (and cleared) where it stays set.
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0x80);
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CCW);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_GREATER);
        glDisable(GL_BLEND);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glStencilFunc(GL_ALWAYS, 0x80, 0x80);
        glCullFace(GL_FRONT);
        glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);
        DrawOne(cmd);
        glCullFace(GL_BACK);
        glStencilOp(GL_KEEP, GL_ZERO, GL_KEEP);
        DrawOne(cmd);
        // the fill through the back faces, without the depth test: the camera may be inside the volume (no front face
        // in view), and a back face covers every pixel whose scene point can be inside
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_DEPTH_TEST);
        glCullFace(GL_FRONT);
        glStencilFunc(GL_EQUAL, 0x80, 0x80);
        glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
        glEnable(GL_BLEND);
        FSetBlend((uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u);
        // (debug: SLY2N_VOLDBG=1: the filled pixels in a colour per volume, red, green, blue, ... in draw order)
        static const bool s_fVolDbg = std::getenv("SLY2N_VOLDBG") != nullptr;
        static int s_ivolDbg = 0;
        static const float s_argbDbg[8][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}, {255, 128, 0}};
        if (s_fVolDbg) { const float* prgb = s_argbDbg[s_ivolDbg++ % 8]; SetGlobRgbaConst(prgb[0], prgb[1], prgb[2], 64.0f); }
        else SetGlobRgbaConst(c.r, c.g, c.b, c.a);
        // (the lists' DATE fill is a flat sprite of (rgba1 + 1) / 2, not faded by the hull's vertex alpha: SLY2N_VOLVA=1
        // fades it for comparison)
        static const bool s_fVa = std::getenv("SLY2N_VOLVA") != nullptr;
        glUniform1i(s_pu->icolor, s_fVa ? 3 : 2);
        DrawOne(cmd);
        glCullFace(GL_BACK);
        glEnable(GL_DEPTH_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        return;
    }
    // A textured volume (light beams): the volume's front faces in front of the scene, filled once per pixel
    // (stencil bit 7), with rgba1 / 2 faded by the vertex alpha; then the bit is cleared again. (The exact P_G
    // 0x3628 / 0x37A8 passes are not reconstructed; this matches the beams PCSX2 shows.)
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x80);
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_NOTEQUAL, 0x80, 0x80);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glEnable(GL_BLEND);
    FSetBlend((uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u);
    SetGlobRgbaConst(c.r, c.g, c.b, c.a);
    glUniform1i(s_pu->icolor, 3);
    DrawOne(cmd);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_ALWAYS, 0, 0x80);
    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
    DrawOne(cmd);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    if (fl & 2)
    {
        // The textured surface of the volume (list 0 with flag 2): blended, no Z write.
        glFrontFace(GL_CCW);
        glDepthFunc(GL_GEQUAL);
        const TEXSEL sel = TexselFromShd(pshd, sub.psaa ? sub.psaa->iframe : 0, 0);
        const GLuint tex = TexFromSel(sel);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(0, s_asampler[SamplerFor(tex, uint8_t((sel.fClampS ? 1 : 0) | (sel.fClampT ? 2 : 0)))]);
        glUniform1i(s_pu->fTextured, 1);
        glUniform1i(s_pu->icolor, 0);
        FSetBlend((uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u);
        DrawOne(cmd);
    }
}

// Whether a point (the hull's space) is inside a type-3 hull: a ray's crossings of its triangles (as BuildGlobGpu
// makes them), odd inside. (The list-1 caps the original adds where the near plane is inside the hull.)
// A type-3 hull's triangles in its own space (as BuildGlobGpu makes them), kept per glob.
const std::vector<glm::vec3>& HullTriangles(const GLOB& glob)
{
    static std::unordered_map<const GLOB*, std::vector<glm::vec3>> s_mpatri;
    auto [it, fNew] = s_mpatri.try_emplace(&glob);
    std::vector<glm::vec3>& atri = it->second;
    if (fNew)
        for (const SUBFX& fx : glob.asubfx)
            for (size_t k = 2; k < fx.aipos.size(); ++k)
            {
                if ((fx.amask[k / 32] >> (k % 32)) & 1) continue;
                for (size_t j = k - 2; j <= k; ++j) atri.push_back(fx.aipos[j] < fx.apos.size() ? fx.apos[fx.aipos[j]] : glm::vec3(0.0f));
            }
    return atri;
}

bool FPointInHull(const GLOB& glob, const glm::vec3& pos)
{
    const std::vector<glm::vec3>& atri = HullTriangles(glob);
    const glm::vec3 dir = glm::normalize(glm::vec3(0.1234f, 0.4567f, 0.8812f));
    int c = 0;
    for (size_t i = 0; i + 2 < atri.size(); i += 3)
    {
        const glm::vec3 e1 = atri[i + 1] - atri[i], e2 = atri[i + 2] - atri[i];
        const glm::vec3 p = glm::cross(dir, e2);
        const float det = glm::dot(e1, p);
        if (std::fabs(det) < 1e-12f) continue;
        const glm::vec3 t = pos - atri[i];
        const float u = glm::dot(t, p) / det;
        if (u < 0.0f || u > 1.0f) continue;
        const glm::vec3 q = glm::cross(t, e1);
        const float v = glm::dot(dir, q) / det;
        if (v < 0.0f || u + v > 1.0f) continue;
        if (glm::dot(e2, q) / det > 0.0f) ++c;
    }
    return c & 1;
}

// Whether the segment a..b crosses the triangle (p0, p1, p2).
bool FSegmentHitsTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2)
{
    const glm::vec3 d = b - a, e1 = p1 - p0, e2 = p2 - p0;
    const glm::vec3 p = glm::cross(d, e2);
    const float det = glm::dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    const glm::vec3 t = a - p0;
    const float u = glm::dot(t, p) / det;
    if (u < 0.0f || u > 1.0f) return false;
    const glm::vec3 q = glm::cross(t, e1);
    const float v = glm::dot(d, q) / det;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float s = glm::dot(e2, q) / det;
    return s >= 0.0f && s <= 1.0f;
}

// Whether the drawn near plane (its four corners, the hull's space) is inside the hull anywhere: a hull triangle cuts
// the near rectangle (a triangle's edge through it or the rectangle's edge through the triangle), or the rectangle lies
// inside (its centre, ray parity).
bool FNearPlaneInHull(const GLOB& glob, const std::array<glm::vec3, 4>& apos)
{
    const std::vector<glm::vec3>& atri = HullTriangles(glob);
    for (size_t i = 0; i + 2 < atri.size(); i += 3)
    {
        const glm::vec3* pt = &atri[i];
        for (int k = 0; k < 3; ++k)
            if (FSegmentHitsTriangle(pt[k], pt[(k + 1) % 3], apos[0], apos[1], apos[2]) ||
                FSegmentHitsTriangle(pt[k], pt[(k + 1) % 3], apos[0], apos[2], apos[3]))
                return true;
        for (int k = 0; k < 4; ++k)
            if (FSegmentHitsTriangle(apos[size_t(k)], apos[size_t((k + 1) % 4)], pt[0], pt[1], pt[2])) return true;
    }
    return FPointInHull(glob, (apos[0] + apos[1] + apos[2] + apos[3]) * 0.25f);
}

// The hull's DATE (TEST DATE without the shader's flag 8).
bool FHullDate(const CMD& cmd) { return !(cmd.prec->pglobGeom->asubfx[0].pshd->grfshd & 8); }

// Shader kind 2 on rp 7 / 8: shadow hulls. The original marks the scene inside them in the frame's green channel
// (0x16DA48 fills it with 0x7E / 0x3E; the hull's faces in front of the scene (list 0, TEST GREATER) subtract their
// colour, +2 or -2 (254) by their facing: the count goes up by one per front face and down by one per back face in
// front of the scene; DATE without the shader's flag 8; the faces the camera's near plane cuts add theirs through list 1,
// ZTST ALWAYS; reference GS log, render.md "shadow frame-channel pass"), and 0x16DD58 darkens the marked pixels by the
// scene's own alpha (ShadowCombine). Natively the same count in the stencil: z-pass (front +1, back -1 where they pass
// the depth test), or z-fail when the drawn near plane is inside the hull anywhere (list 1's near-plane caps: the same
// count for a closed hull). Only the hulls the near plane reaches: a z-fail count for every hull near the camera marked
// open hulls' insides the original leaves alone; a camera-inside test missed the hulls the near plane cuts with the
// camera just outside (R47: a screen-tall unmarked band, the hull's part behind the near plane uncounted).
// fDateMarked: the stencil's STENCIL_FBA holds the phase's DATE (FMarkDate), so the program need not test it.
void DrawShadow(const CMD& cmd, const glm::vec3& posCamera, float sNearClip, const std::array<glm::vec3, 4>& aposNear, bool fDateMarked)
{
    const GLOB& geom = *cmd.prec->pglobGeom;
    const GLuint maskCount = cmd.prec->kind == 8 ? STENCIL_COUNT8 : STENCIL_COUNT7;
    bool fNear = false;
    if (geom.pgl && geom.pgl->sRadius > 0.0f)
    {
        const glm::mat4& mat = *cmd.prec->pmat;
        const float scale = std::max({glm::length(glm::vec3(mat[0])), glm::length(glm::vec3(mat[1])), glm::length(glm::vec3(mat[2]))});
        const glm::vec3 c = glm::vec3(mat * glm::vec4(geom.pgl->posCenter, 1.0f));
        float sNearExtent = sNearClip;
        for (const glm::vec3& pos : aposNear) sNearExtent = std::max(sNearExtent, glm::distance(posCamera, pos));
        if (glm::distance(posCamera, c) < geom.pgl->sRadius * scale + sNearExtent)
        {
            const glm::mat4 matInv = glm::inverse(mat);
            std::array<glm::vec3, 4> apos;
            for (size_t i = 0; i < 4; ++i) apos[i] = glm::vec3(matInv * glm::vec4(aposNear[i], 1.0f));
            static const bool s_fCamOnly = std::getenv("SLY2N_SHNEARCAM") != nullptr;   // (debug: the camera-inside test only)
            fNear = s_fCamOnly ? glm::distance(posCamera, c) < geom.pgl->sRadius * scale + sNearClip &&
                                     FPointInHull(geom, glm::vec3(matInv * glm::vec4(posCamera, 1.0f)))
                               : FNearPlaneInHull(geom, apos);
        }
    }
    static const int s_iShMode = std::getenv("SLY2N_SHCOUNT") ? std::atoi(std::getenv("SLY2N_SHCOUNT")) : 0;   // (debug: 1 z-pass, 2 z-fail)
    if (s_iShMode) fNear = s_iShMode == 2;
    const bool fDate = FHullDate(cmd);
    if (static const bool s_fDbg = std::getenv("SLY2N_SHHULLDBG") != nullptr; s_fDbg)
        std::printf("shhull: oid %d glob %zu kind %d grfshd 0x%X near %d tris %u r %.0f\n", cmd.prec->psgs ? cmd.prec->psgs->oid : -1, cmd.prec->iglob,
                    cmd.prec->kind, unsigned(geom.asubfx[0].pshd->grfshd), int(fNear), cmd.count / 3, geom.pgl ? geom.pgl->sRadius : -1.0f);
    glBindVertexArray(cmd.vao);
    glUniform1i(s_pu->fTextured, 0);
    glUniform1i(s_pu->ate, 0);
    glUniform1i(s_pu->afailMode, 0);
    SetGlobRgbaConst(0, 0, 0, 0);
    glUniform1i(s_pu->icolor, 2);
    // (DATE: the alpha's bit 7 clear: the FBA bit, or alpha >= 0x80; with the mark, the stencil bit alone)
    glUniform1i(s_pu->fDate, fDate && !fDateMarked ? 1 : 0);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(maskCount);
    glStencilFunc(GL_EQUAL, 0, fDate ? STENCIL_FBA : 0);
    if (fNear)
    {
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
    }
    else
    {
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
    }
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    DrawOne(cmd);
    glUniform1i(s_pu->fDate, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
}

// 0x16DA48 (AddScreenEffect) at a shadow phase's start: the frame as the scene left it (the original copies its green
// plane away, 0x16D6C8 0x299, and fills it with the marks' base); natively its alpha into a texture the hulls' DATE
// and the combine read (resolved when multisampled).
GLuint s_fboShAlpha = 0, s_texShAlpha = 0;
int s_dxShAlpha = 0, s_dyShAlpha = 0;
GLSHADER s_shShadowCombine;
#ifdef __SWITCH__
// (the Switch) The frame rectangle a shadow phase's volumes can reach (x, y, width, height; set by RenderPass at the
// phase's start): the frame copy, the DATE mark and the combine's ramps only there (the marks are under the volumes).
std::array<int, 4> s_rectShPhase = {0, 0, 0, 0};
#endif
void ShadowBegin(GLuint fbo, int dx, int dy, int kind)
{
    if (!s_fboShAlpha || dx != s_dxShAlpha || dy != s_dyShAlpha)
    {
        if (!s_fboShAlpha) { glGenFramebuffers(1, &s_fboShAlpha); glGenTextures(1, &s_texShAlpha); }
        glBindTexture(GL_TEXTURE_2D, s_texShAlpha);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dx, dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShAlpha);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_texShAlpha, 0);
        s_dxShAlpha = dx;
        s_dyShAlpha = dy;
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShAlpha);
#ifdef __SWITCH__
    {
        const std::array<int, 4>& r = s_rectShPhase;
        if (r[2] > 0 && r[3] > 0)
            glBlitFramebuffer(r[0], r[1], r[0] + r[2], r[1] + r[3], r[0], r[1], r[0] + r[2], r[1] + r[3], GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
#else
    glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_COLOR_BUFFER_BIT, GL_NEAREST);
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_texShAlpha);
    glActiveTexture(GL_TEXTURE0);
    // the count's base (the green fill)
    glStencilMask(STENCIL_COUNT7);
    glClearStencil(kind == 8 ? 0x1F : 0x3F);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
    // (debug: SLY2N_SHALPHA=dir writes each phase's frame alpha x 2 as a grey PNG)
    if (static const char* s_szDir = std::getenv("SLY2N_SHALPHA"); s_szDir)
    {
        static int s_c = 0;
        std::vector<uint8_t> ab(size_t(dx) * size_t(dy) * 4), ag(size_t(dx) * size_t(dy));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fboShAlpha);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, dx, dy, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        for (int y = 0; y < dy; ++y)
            for (int x = 0; x < dx; ++x) ag[size_t(dy - 1 - y) * size_t(dx) + size_t(x)] = ab[(size_t(y) * size_t(dx) + size_t(x)) * 4 + 3];
        stbi_write_png((std::string(s_szDir) + "/shalpha_" + std::to_string(s_c++) + ".png").c_str(), dx, dy, 1, ag.data(), dx);
    }
}

// A phase's DATE as a stencil mark: STENCIL_FBA also set where the frame's alpha at the phase's start (ShadowBegin's
// copy) is 0x80 or more, the discard's test, so the hulls and type-2 volumes test the bit alone, with programs that
// never discard. False: no mark (the program did not build), the DATE discard instead.
GLSHADER s_shShadowDate;
bool FMarkDate()
{
    static int s_state = 0;   // 0: not tried yet, 1: available, -1: not
    if (s_state == 0)
        s_state = glGlobShaderNoKill.ID && s_shShadowDate.FInit("shadowcombine.vert", "shadowdate.frag", "") ? 1 : -1;
    if (s_state < 0 || !s_texShAlpha) return false;
    s_shShadowDate.Use();
    glUniform1i(s_shShadowDate.Loc("texDate"), 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_texShAlpha);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vaoEmpty);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(STENCIL_FBA);
    glStencilFunc(GL_ALWAYS, GLint(STENCIL_FBA), STENCIL_FBA);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
#ifdef __SWITCH__
    glEnable(GL_SCISSOR_TEST);   // (the phase's volumes' rectangle: s_rectShPhase)
    glScissor(s_rectShPhase[0], s_rectShPhase[1], s_rectShPhase[2], s_rectShPhase[3]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_SCISSOR_TEST);
#else
    glDrawArrays(GL_TRIANGLES, 0, 3);
#endif
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    return true;
}

// 0x16DA48 for kind 8 after kind 7: the kind-8 count's base, kind 7's mark (bit 6) kept (FBMSK 0xFFFF80FF).
void ShadowKind8()
{
    glStencilMask(STENCIL_COUNT8);
    glClearStencil(0x1F);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
}

// Past the FBA kinds without a shadow phase: the stencil's FBA bits cleared (the volumes' mask, kinds 11 / 13, is bit 7).
void ClearFbaBits()
{
    glStencilMask(STENCIL_FBA);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
}

#if !defined(__SWITCH__) && !defined(__APPLE__)
// PC soft shadows (display.h softShadows, shadowsoft.frag): a single-sample copy of the frame's depth and stencil (one
// storage, read through two views: the depth, and the stencil's marks), at the shadow phase's frame size.
GLuint s_fboShDS = 0, s_texShDS = 0, s_texShStencil = 0;
int s_dxShDS = 0, s_dyShDS = 0;
GLSHADER s_shShadowSoft;
int s_stateShadowSoft = 0;   // 0 not yet, 1 built, -1 failed (the original's hard edges)

// (also the character shadow maps' copy of the depth, ApplyShadowMaps)
int s_stateShDS = 0;   // 0 not yet, 1 usable, -1 not (the framebuffer was incomplete)
bool FEnsureDsCopy(int dx, int dy)
{
    if (s_stateShDS < 0) return false;
    if (s_fboShDS && dx == s_dxShDS && dy == s_dyShDS) return true;
    if (s_fboShDS) glDeleteFramebuffers(1, &s_fboShDS), s_fboShDS = 0;
    if (s_texShStencil) glDeleteTextures(1, &s_texShStencil), s_texShStencil = 0;
    if (s_texShDS) glDeleteTextures(1, &s_texShDS), s_texShDS = 0;
    // (unit 3, put back by the caller: the renderer's units 0 and 1 stay as they are)
    glActiveTexture(GL_TEXTURE3);
    glGenTextures(1, &s_texShDS);
    glBindTexture(GL_TEXTURE_2D, s_texShDS);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH32F_STENCIL8, dx, dy);   // (the frame's format: a blit needs the same)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
    glGenTextures(1, &s_texShStencil);
    glTextureView(s_texShStencil, GL_TEXTURE_2D, s_texShDS, GL_DEPTH32F_STENCIL8, 0, 1, 0, 1);
    glBindTexture(GL_TEXTURE_2D, s_texShStencil);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    GLint fboPrev = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboPrev);
    glGenFramebuffers(1, &s_fboShDS);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShDS);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_texShDS, 0);
    glDrawBuffer(GL_NONE);
    const bool fOk = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fboPrev));
    if (!fOk)
    {
        std::printf("soft shadows / shadow maps: the depth-stencil copy is not usable here: the original's shadows\n");
        s_stateShDS = -1;
        return false;
    }
    s_stateShDS = 1;
    s_dxShDS = dx;
    s_dyShDS = dy;
    return true;
}

bool FEnsureShadowSoft(int dx, int dy)
{
    if (s_stateShadowSoft == 0)
        s_stateShadowSoft = s_shShadowSoft.FInit("shadowcombine.vert", "shadowsoft.frag", "") ? 1 : -1;
    return s_stateShadowSoft > 0 && FEnsureDsCopy(dx, dy);
}
#endif

// 0x16DD58 (AddScreenCombine) at the phase's end: the unmarked pixels' alpha (entry 0), then the CLUT pass
// (shadowcombine.frag), one draw per ramp the marks make (their pixels are disjoint: one pass over the frame in all);
// the marks are cleared. f7 / f8: the phase drew kind 7 / kind 8.
void ShadowCombine(bool f7, bool f8)
{
    // (debug: SLY2N_SHMARKS=dir writes each combine's marks: red kind 7, green kind 8, blue the FBA bit; no MSAA)
    if (static const char* s_szDir = std::getenv("SLY2N_SHMARKS"); s_szDir && s_dxShAlpha > 0)
    {
        static int s_c = 0;
        const int dx = s_dxShAlpha, dy = s_dyShAlpha;
        std::vector<uint8_t> as(size_t(dx) * size_t(dy)), argb(size_t(dx) * size_t(dy) * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, dx, dy, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, as.data());
        for (int y = 0; y < dy; ++y)
            for (int x = 0; x < dx; ++x)
            {
                const uint8_t v = as[size_t(y) * size_t(dx) + size_t(x)];
                uint8_t* p = &argb[(size_t(dy - 1 - y) * size_t(dx) + size_t(x)) * 3];
                p[0] = (v & STENCIL_SH7) ? 255 : 0;
                p[1] = f8 && (v & STENCIL_SH8) ? 255 : 0;
                p[2] = (v & STENCIL_FBA) ? 128 : 0;
            }
        stbi_write_png((std::string(s_szDir) + "/shmarks_" + std::to_string(s_c++) + ".png").c_str(), dx, dy, 3, argb.data(), dx * 3);
    }
    // (PC soft shadows: the marks and the depth copied out before the combine reads and clears them)
    bool fSoft = false;
#if !defined(__SWITCH__) && !defined(__APPLE__)
    if (g_display.softShadows > 0 && FEnsureShadowSoft(s_dxShAlpha, s_dyShAlpha))
    {
        GLint fbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
        while (glGetError() != GL_NO_ERROR) {}
        glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(fbo));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShDS);
        glBlitFramebuffer(0, 0, s_dxShAlpha, s_dyShAlpha, 0, 0, s_dxShAlpha, s_dyShAlpha, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                          GL_NEAREST);
        fSoft = glGetError() == GL_NO_ERROR;
        glBindFramebuffer(GL_FRAMEBUFFER, GLuint(fbo));
    }
    GLint texUnit2 = 0, texUnit3 = 0, samplerUnit2 = 0, samplerUnit3 = 0;
#endif
    if (!s_shShadowCombine.ID) s_shShadowCombine.Init("shadowcombine.vert", "shadowcombine.frag");
    s_shShadowCombine.Use();
    glUniform1i(s_shShadowCombine.Loc("texAlpha"), 1);
    const GLint locGain = s_shShadowCombine.Loc("gain"), locM8 = s_shShadowCombine.Loc("m8"), locReset = s_shShadowCombine.Loc("fReset");
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_texShAlpha);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vaoEmpty);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    // The unmarked pixels' alpha (the CLUT's entry 0) as one alpha-only clear before the ramps, not a stencil-tested
    // draw after them: each marked pixel is in a ramp's class, and a ramp sets its alpha whatever the frame's (A = As).
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, s_dxShAlpha, s_dyShAlpha);   // (the pass's frame, as the draw's viewport covered)
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);   // (0 - Cd) x As / 128 + Cd; A = As
#if !defined(__SWITCH__) && !defined(__APPLE__)
    if (fSoft)
    {
        // the colour: the ramps' darkening averaged over a disc, every pixel at once (no stencil test); the ramps
        // below then set only the alpha, as the original's
        glActiveTexture(GL_TEXTURE2);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texUnit2);
        glGetIntegerv(GL_SAMPLER_BINDING, &samplerUnit2);
        glBindTexture(GL_TEXTURE_2D, s_texShStencil);
        glBindSampler(2, 0);
        glActiveTexture(GL_TEXTURE3);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texUnit3);
        glGetIntegerv(GL_SAMPLER_BINDING, &samplerUnit3);
        glBindTexture(GL_TEXTURE_2D, s_texShDS);
        glBindSampler(3, 0);
        glActiveTexture(GL_TEXTURE0);
        s_shShadowSoft.Use();
        glUniform1i(s_shShadowSoft.Loc("texAlpha"), 1);
        glUniform1i(s_shShadowSoft.Loc("texStencil"), 2);
        glUniform1i(s_shShadowSoft.Loc("texDepth"), 3);
        glUniform1ui(s_shShadowSoft.Loc("maskMarks"), f8 ? STENCIL_SH7 | STENCIL_SH8 : STENCIL_SH7);
        glUniform1ui(s_shShadowSoft.Loc("stencil7"), STENCIL_SH7);
        glUniform1ui(s_shShadowSoft.Loc("stencil8"), STENCIL_SH8);
        glUniform3f(s_shShadowSoft.Loc("gain"), 128.0f / 255.0f, 96.0f / 255.0f, 160.0f / 255.0f);
        glUniform3i(s_shShadowSoft.Loc("framp"), f7 ? 1 : 0, f8 ? 1 : 0, f7 && f8 ? 1 : 0);
        // (about 2 or 4 PS2 pixels)
        glUniform1f(s_shShadowSoft.Loc("radius"), float(s_dyShAlpha) / 448.0f * (g_display.softShadows >= 2 ? 4.0f : 2.0f));
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_STENCIL_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        s_shShadowCombine.Use();
    }
#endif
    glUniform1i(locReset, 0);
    // (GAME +0x3E64 / +0x3E70, 0x1EA9F0: 128 / 96 / 160 and black in every world's RAM seen)
    struct RAMP { GLuint stencil; float gain; int m8; bool f; };
    const RAMP aramp[] = {{STENCIL_SH7, 128.0f, 0, f7}, {STENCIL_SH8, 96.0f, 1, f8}, {STENCIL_SH7 | STENCIL_SH8, 160.0f, 1, f7 && f8}};
    const GLuint maskMarks = f8 ? STENCIL_SH7 | STENCIL_SH8 : STENCIL_SH7;   // (without kind 8, bit 5 is kind 7's count)
#ifdef __SWITCH__
    glEnable(GL_SCISSOR_TEST);   // (the marks are within the phase's volumes' rectangle: s_rectShPhase)
    glScissor(s_rectShPhase[0], s_rectShPhase[1], s_rectShPhase[2], s_rectShPhase[3]);
#endif
    for (const RAMP& r : aramp)
    {
        if (!r.f) continue;
        glStencilFunc(GL_EQUAL, GLint(r.stencil), maskMarks);
        glUniform1f(locGain, r.gain / 255.0f);
        glUniform1i(locM8, r.m8);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
#ifdef __SWITCH__
    glDisable(GL_SCISSOR_TEST);
#endif
#if !defined(__SWITCH__) && !defined(__APPLE__)
    if (fSoft)
    {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, GLuint(texUnit3));
        glBindSampler(3, GLuint(samplerUnit3));
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, GLuint(texUnit2));
        glBindSampler(2, GLuint(samplerUnit2));
        glActiveTexture(GL_TEXTURE0);
    }
#endif
    glDisable(GL_BLEND);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
}

#if !defined(__SWITCH__) && !defined(__APPLE__)
// ------------------------------------------------------------ PC: character shadow maps (TEST, display.h shadowMaps)
// The type-2 casters of a pass (render kind 7: the characters' cast shadows), each with its shadow's light: the
// original's ShadowOfLightContext centre (its strongest shadow lights' weighted place; a directional light 1000 back)
// looking at the caster. Up to four, nearest the camera first, in 2 x 2 tiles of one depth atlas.
constexpr int kcSmTile = 1024, kcSmCasterMax = 4;
struct SMCASTER
{
    const SCENEGLOBSET* psgs = nullptr;
    glm::mat4 matLight{1.0f};   // world to the tile's clip (standard depth, [0, 1])
    glm::vec4 c{0.0f};          // shadowmap.frag acaster
    float distCamera = 0;
};
std::vector<SMCASTER> s_asmcaster;
GLSHADER s_shSmDepth, s_shSmApply;
int s_stateSm = 0;   // 0 not yet, 1 built, -1 failed (the volumes)
GLuint s_fboSm = 0, s_texSm = 0, s_ssboSm = 0;

bool FShadowMapsReady()
{
    if (s_stateSm == 0)
    {
        const bool f = s_shSmDepth.FInit("smdepth.vert", "smdepth.frag", "") && s_shSmApply.FInit("shadowcombine.vert", "shadowmap.frag", "");
        if (f)
        {
            GLint texPrev = 0, fboPrev = 0, activePrev = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &activePrev);
            glActiveTexture(GL_TEXTURE4);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texPrev);
            glGenTextures(1, &s_texSm);
            glBindTexture(GL_TEXTURE_2D, s_texSm);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, 2 * kcSmTile, 2 * kcSmTile);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   // (2 x 2 compared taps per lookup)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
            glBindTexture(GL_TEXTURE_2D, GLuint(texPrev));
            glActiveTexture(GLenum(activePrev));
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboPrev);
            glGenFramebuffers(1, &s_fboSm);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboSm);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_texSm, 0);
            glDrawBuffer(GL_NONE);
            const bool fOk = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fboPrev));
            glGenBuffers(1, &s_ssboSm);
            s_stateSm = fOk ? 1 : -1;
        }
        else s_stateSm = -1;
        std::printf("shadow maps (TEST): %s\n", s_stateSm > 0 ? "on" : "did not build: the original's volumes");
    }
    return s_stateSm > 0;
}

// A caster's light, from the world positions of its shadow hulls (as FBuildShadow2 skins them): false when its shadow
// has no light (FBuildShadow2 would draw nothing either).
bool FSmCasterOf(const SCENEGLOBSET& sgs, const std::vector<glm::vec3>& av, const glm::vec3& posCamera, SMCASTER* pc)
{
    if (av.empty()) return false;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const glm::vec3& v : av) lo = glm::min(lo, v), hi = glm::max(hi, v);
    const glm::vec3 posCtx = sgs.fPosLight ? sgs.posLight : (lo + hi) * 0.5f;
    float sRadius = 0;
    for (const glm::vec3& v : av) sRadius = std::max(sRadius, glm::length(v - posCtx));
    const SHADOWCTX sh = ShadowOfLightContext(WorldLights(), posCtx, sRadius, 400.0f, posCamera, 52.0f);
    if (!sh.fValid || sRadius <= 0.0f) return false;
    const glm::vec3 d = posCtx - sh.center;
    const float dist = glm::length(d);
    if (dist < 1.0f) return false;
    const glm::vec3 dir = d / dist;
    const glm::vec3 up = std::fabs(dir.z) < 0.95f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    // the caster just fills its tile; the shadow reaches about as far as the original's long shadows
    const float r = sRadius * 1.15f;
    const float sNearL = std::max(dist - r, 1.0f), sFarL = dist + r + 2500.0f;
    const float fovy = 2.0f * std::atan(r / dist);
    pc->psgs = &sgs;
    pc->matLight = glm::perspectiveRH_ZO(fovy, 1.0f, sNearL, sFarL) * glm::lookAt(sh.center, posCtx, up);
    // (the depth 600 short of the far plane, where the shadow starts fading out)
    const float dFade = sFarL - 600.0f;
    const float zFade = sFarL * (dFade - sNearL) / (dFade * (sFarL - sNearL));
    pc->c = glm::vec4(lo.z - 120.0f, 80.0f, zFade, 0.0f);
    pc->distCamera = glm::length(posCtx - posCamera);
    return true;
}

// The GL state the two passes change, saved and put back (the renderer's cache is invalidated after them too).
struct SMSAVE
{
    GLint prog = 0, vao = 0, fboDraw = 0, fboRead = 0, activeTex = 0, viewport[4] = {}, depthFunc = 0, stencilMask = 0;
    GLint tex3 = 0, tex4 = 0, sampler3 = 0, sampler4 = 0;
    GLint blendSrcRgb = 0, blendDstRgb = 0, blendSrcA = 0, blendDstA = 0, blendEqRgb = 0, blendEqA = 0;
    GLfloat clearDepth = 0;
    GLboolean fBlend = 0, fDepth = 0, fStencil = 0, fCull = 0, fScissor = 0, fOffset = 0, fDepthMask = 0, colorMask[4] = {};
    SMSAVE()
    {
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboDraw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fboRead);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
        glActiveTexture(GL_TEXTURE3);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex3);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler3);
        glActiveTexture(GL_TEXTURE4);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex4);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler4);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEqRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqA);
        glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clearDepth);
        fBlend = glIsEnabled(GL_BLEND), fDepth = glIsEnabled(GL_DEPTH_TEST), fStencil = glIsEnabled(GL_STENCIL_TEST);
        fCull = glIsEnabled(GL_CULL_FACE), fScissor = glIsEnabled(GL_SCISSOR_TEST), fOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &fDepthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    }
    ~SMSAVE()
    {
        auto set = [](GLenum cap, GLboolean f) { if (f) glEnable(cap); else glDisable(cap); };
        glUseProgram(GLuint(prog));
        glBindVertexArray(GLuint(vao));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fboDraw));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(fboRead));
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, GLuint(tex4));
        glBindSampler(4, GLuint(sampler4));
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, GLuint(tex3));
        glBindSampler(3, GLuint(sampler3));
        glActiveTexture(GLenum(activeTex));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glDepthFunc(GLenum(depthFunc));
        glStencilMask(GLuint(stencilMask));
        glBlendFuncSeparate(GLenum(blendSrcRgb), GLenum(blendDstRgb), GLenum(blendSrcA), GLenum(blendDstA));
        glBlendEquationSeparate(GLenum(blendEqRgb), GLenum(blendEqA));
        glClearDepth(clearDepth);
        set(GL_BLEND, fBlend), set(GL_DEPTH_TEST, fDepth), set(GL_STENCIL_TEST, fStencil), set(GL_CULL_FACE, fCull);
        set(GL_SCISSOR_TEST, fScissor), set(GL_POLYGON_OFFSET_FILL, fOffset);
        glDepthMask(fDepthMask);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    }
};

// The casters' meshes (their visible strip groups, as RenderGhostPoses draws a pose) into their tiles of the atlas.
void RenderShadowMaps()
{
    static std::vector<glm::mat4> s_amatBone;
    struct SMDRAW { GLuint vao; int32_t baseVertex; uint32_t firstIndex, count; glm::mat4 mat; uint32_t iboneFirst, cbone; size_t icaster; };
    static std::vector<SMDRAW> s_ad;
    s_amatBone.clear();
    s_ad.clear();
    for (size_t ic = 0; ic < s_asmcaster.size(); ++ic)
    {
        const SCENEGLOBSET& sgs = *s_asmcaster[ic].psgs;
        if (!sgs.pglobset) continue;
        const GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            if (i >= sgs.afVisible.size() || !sgs.afVisible[i] || i >= sgs.amatGlob.size()) continue;
            const GLOB& glob = gs.aglob[i];
            const GLOB* pgeom = &glob;
            if (!glob.pgl && glob.iglobTemplate >= 0 && size_t(glob.iglobTemplate) < gs.aglob.size()) pgeom = &gs.aglob[size_t(glob.iglobTemplate)];
            if (pgeom->meshk != MESHK_Strips || !pgeom->pgl) continue;
            const GLGLOB* pgl = i < sgs.apglInst.size() && sgs.apglInst[i] && sgs.apglInst[i]->vao ? sgs.apglInst[i] : pgeom->pgl;
            uint32_t iboneFirst = 0, cbone = 0;
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty())
            {
                iboneFirst = uint32_t(s_amatBone.size());
                cbone = uint32_t(sgs.aamatBone[i].size());
                s_amatBone.insert(s_amatBone.end(), sgs.aamatBone[i].begin(), sgs.aamatBone[i].end());
            }
            for (const SUBGLOB& sub : pgeom->asubglob)
            {
                if (!sub.pshd || sub.indexCount == 0 || sub.pshd->shdk == 3 || sub.pshd->apass.empty()) continue;
                // (no glows: the parts drawn added cast nothing)
                const SHDPASS& ps = sub.pshd->apass[0];
                if (ps.fBlend && ((ps.alpha >> 2) & 3) == 2 && ((ps.alpha >> 6) & 3) == 1) continue;
                s_ad.push_back({pgl->vao, pgl->baseVertex, pgl->firstIndex + sub.firstIndex, sub.indexCount, sgs.amatGlob[i], iboneFirst, cbone, ic});
            }
        }
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboSm);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    if (s_ad.empty()) return;
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);   // (the meshes' winding is not to be relied on: both sides)
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.0f);
    s_shSmDepth.Use();
    const GLint locLight = s_shSmDepth.Loc("matWorldToLight"), locModel = s_shSmDepth.Loc("matModel");
    const GLint locBoneFirst = s_shSmDepth.Loc("iboneFirst"), locCBone = s_shSmDepth.Loc("cbone");
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssboSm);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(std::max<size_t>(1, s_amatBone.size()) * sizeof(glm::mat4)),
                 s_amatBone.empty() ? nullptr : s_amatBone.data(), GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, s_ssboSm);
    size_t icCur = size_t(-1);
    GLuint vaoCur = 0;
    for (const SMDRAW& d : s_ad)
    {
        if (d.icaster != icCur)
        {
            icCur = d.icaster;
            glViewport(GLint(icCur & 1) * kcSmTile, GLint(icCur >> 1) * kcSmTile, kcSmTile, kcSmTile);
            glUniformMatrix4fv(locLight, 1, GL_FALSE, glm::value_ptr(s_asmcaster[icCur].matLight));
        }
        if (d.vao != vaoCur) glBindVertexArray(vaoCur = d.vao);
        glUniform1i(locCBone, GLint(d.cbone));
        glUniform1i(locBoneFirst, GLint(d.iboneFirst));
        if (!d.cbone) glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(d.mat));
        glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(d.count), GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(d.firstIndex) * 4), d.baseVertex);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

// The shadows on the frame (fbo, in a shadow phase: its alpha copy on unit 1), from the atlas and the depth.
void ApplyShadowMaps(GLuint fbo, const glm::mat4& matWorldToClip)
{
    const int dx = s_dxShAlpha, dy = s_dyShAlpha;
    if (!FEnsureDsCopy(dx, dy)) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShDS);
    glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, dx, dy);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, s_texShDS);
    glBindSampler(3, 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, s_texSm);
    glBindSampler(4, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_texShAlpha);
    glActiveTexture(GL_TEXTURE0);
    s_shSmApply.Use();
    glUniform1i(s_shSmApply.Loc("texAlpha"), 1);
    glUniform1i(s_shSmApply.Loc("texDepth"), 3);
    glUniform1i(s_shSmApply.Loc("texSm"), 4);
    const glm::mat4 matClipToWorld = glm::inverse(matWorldToClip);
    glUniformMatrix4fv(s_shSmApply.Loc("matClipToWorld"), 1, GL_FALSE, glm::value_ptr(matClipToWorld));
    glm::mat4 amat[kcSmCasterMax];
    glm::vec4 ac[kcSmCasterMax];
    const int c = int(std::min<size_t>(s_asmcaster.size(), kcSmCasterMax));
    for (int i = 0; i < c; ++i) amat[i] = s_asmcaster[size_t(i)].matLight, ac[i] = s_asmcaster[size_t(i)].c;
    glUniformMatrix4fv(s_shSmApply.Loc("amatLight"), c, GL_FALSE, glm::value_ptr(amat[0]));
    glUniform4fv(s_shSmApply.Loc("acaster"), c, glm::value_ptr(ac[0]));
    glUniform1i(s_shSmApply.Loc("ccaster"), c);
    glUniform2f(s_shSmApply.Loc("viewSize"), float(dx), float(dy));
    glUniform1f(s_shSmApply.Loc("rcpAtlas"), 1.0f / float(2 * kcSmTile));
    glUniform1f(s_shSmApply.Loc("gain"), 128.0f / 255.0f);
    glBindVertexArray(s_vaoEmpty);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);   // (the colour darkened; the alpha kept)
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void DrawShadowMaps(const glm::mat4& matWorldToClip)
{
    if (s_asmcaster.empty() || !FShadowMapsReady()) return;
    SMSAVE save;
    GLuint fbo = GLuint(save.fboDraw);
    RenderShadowMaps();
    ApplyShadowMaps(fbo, matWorldToClip);
}

// ------------------------------------------------------------ PC: the added passes' GPU times
// (render.h StrAddedGpuTimes: the debug menu's Perf Display) Each added pass in a GL_TIME_ELAPSED query, three frames of
// queries in turn, each read when the GPU has it (no stall); a pass not run fades out of the line.
struct AGTIMER { GLuint aq[3] = {}; bool afPending[3] = {}; double ms = 0.0; bool fRun = false; };
AGTIMER s_aagt[AGT_Max];
int s_iagtSlot = 0, s_agtActive = -1;

void StepAddedGpuTimers()
{
    for (AGTIMER& t : s_aagt)
    {
        for (int k = 0; k < 3; ++k)
        {
            if (!t.afPending[k]) continue;
            GLint fAvail = 0;
            glGetQueryObjectiv(t.aq[k], GL_QUERY_RESULT_AVAILABLE, &fAvail);
            if (!fAvail) continue;
            GLuint64 ns = 0;
            glGetQueryObjectui64v(t.aq[k], GL_QUERY_RESULT, &ns);
            t.ms = t.ms * 0.9 + 0.1 * double(ns) / 1e6;
            t.afPending[k] = false;
        }
        if (!t.fRun) t.ms *= 0.9;
        t.fRun = false;
    }
    s_iagtSlot = (s_iagtSlot + 1) % 3;
}

void BeginAddedGpuTimer(AGT agt)
{
    if (!g_fAddedGpuTimes || s_agtActive >= 0) return;
    AGTIMER& t = s_aagt[agt];
    if (!t.aq[0]) glGenQueries(3, t.aq);
    t.fRun = true;
    if (t.afPending[s_iagtSlot]) return;   // (its last use not read yet)
    glBeginQuery(GL_TIME_ELAPSED, t.aq[s_iagtSlot]);
    s_agtActive = int(agt);
}

void EndAddedGpuTimer(AGT agt)
{
    if (s_agtActive != int(agt)) return;
    glEndQuery(GL_TIME_ELAPSED);
    s_aagt[agt].afPending[s_iagtSlot] = true;
    s_agtActive = -1;
}

// ------------------------------------------------------------ PC: world shadows (TEST, display.h worldShadows)
// The level's static meshes into one orthographic depth map from its main directional light (the strongest
// directional shadow light of the world's light context, else of the bake's lights; SLY2N_WORLDSHADOWDIR "x y z" over
// both; none: a light from high above), a box of kcWsRadius round the point ahead of the camera, its centre snapped to
// the map's texels (no shimmer as the camera moves); then every pixel of the frame looked up in it (wsapply.frag).
// Not cast by: skinned groups (the characters: their own shadows), additive glows, shaders of kind 3, objects drawn
// into render targets or over the HUD, faded-out objects and the sky (anything wider than kcWsSkyRadius).
constexpr int kcWsMap = 4096;
constexpr float kcWsRadius = 2600.0f, kcWsDepth = 9000.0f, kcWsSkyRadius = 40000.0f;
// (one group of an object wider than kcWsSkyRadius wider than this: the sky's dome, its clouds' ring)
constexpr float kcWsSkyGlobRadius = 12000.0f;
struct WSBOX { glm::vec3 L{0.0f, 0.0f, -1.0f}, cWorld{0.0f}; glm::mat4 matLight{1.0f}; float texel = 1.0f; };
GLSHADER s_shWsDepth, s_shWsApply;
int s_stateWs = 0;   // 0 not yet, 1 built, -1 failed (no world shadows)
GLuint s_fboWs = 0, s_texWs = 0, s_ssboWs = 0;   // (the skinned casters' bones)
// (the map of the casters that stay still, kept while nothing it was drawn from changes: s_texWs is it with the moving
// casters drawn over a copy, each frame)
GLuint s_fboWsStatic = 0, s_texWsStatic = 0;
uint64_t s_keyWsStatic = 0;
int s_cWsStaticDrawn = 0;   // (how often it was drawn: SLY2N_WSPRINT)
// (PC) The characters' own pixels this frame (render kinds 4 / 6, the FBA kinds the original's shadows keep off: Sly,
// the gang, the guards), drawn into an R8 mask from the camera against a copy of the frame's depth: the world shadows
// and the ambient occlusion leave them alone. Built after the frame pass when either is on (RenderScene).
GLSHADER s_shCharMask;
int s_stateCharMask = 0;   // 0 not yet, 1 built, -1 failed (no mask: they draw over the characters as before)
GLuint s_fboCharMask = 0, s_texCharMask = 0, s_ssboCharMask = 0, s_texDSMasked = 0;
int s_dxCharMask = 0, s_dyCharMask = 0;
bool s_fCharMaskFrame = false;

bool FEnsureCharMask(int dx, int dy)
{
    if (s_stateCharMask == 0)
    {
        s_stateCharMask = -1;
        // (smdepth.vert, and a fragment program that marks)
        if (s_shCharMask.FInit("smdepth.vert", "charmask.frag", "")) s_stateCharMask = 1, glGenBuffers(1, &s_ssboCharMask);
        std::printf("character mask: %s\n", s_stateCharMask > 0 ? "built" : "did not build: AO and the level's shadows draw over the characters");
    }
    if (s_stateCharMask < 0 || !FEnsureDsCopy(dx, dy)) return false;
    if (s_fboCharMask && dx == s_dxCharMask && dy == s_dyCharMask && s_texDSMasked == s_texShDS) return true;
    if (s_fboCharMask) glDeleteFramebuffers(1, &s_fboCharMask), s_fboCharMask = 0;
    if (s_texCharMask) glDeleteTextures(1, &s_texCharMask), s_texCharMask = 0;
    GLint activePrev = 0, texPrev = 0, fboPrev = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activePrev);
    glActiveTexture(GL_TEXTURE5);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texPrev);
    glGenTextures(1, &s_texCharMask);
    glBindTexture(GL_TEXTURE_2D, s_texCharMask);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, dx, dy);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, GLuint(texPrev));
    glActiveTexture(GLenum(activePrev));
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboPrev);
    glGenFramebuffers(1, &s_fboCharMask);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboCharMask);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_texCharMask, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_texShDS, 0);
    const GLenum buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buf);
    const bool fOk = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fboPrev));
    if (!fOk)
    {
        std::printf("character mask: its framebuffer is not usable here\n");
        s_stateCharMask = -1;
        return false;
    }
    s_dxCharMask = dx;
    s_dyCharMask = dy;
    s_texDSMasked = s_texShDS;
    return true;
}

// the render kinds of the characters' groups (the FBA kinds: 4, faded 6)
bool FCharacterGlob(const GLOB& glob) { const int kind = glob.rp & 0x1F; return kind == 4 || kind == 6; }

bool FBuildCharacterMask(const SCENE& scene, const glm::mat4& matWorldToClip, const glm::vec3& posEye, GLuint fbo, int dx, int dy)
{
    if (!FEnsureCharMask(dx, dy)) return false;
    struct MDRAW { GLuint vao; int32_t baseVertex; uint32_t firstIndex, count; const glm::mat4* pmat; uint32_t iboneFirst, cbone; };
    static std::vector<MDRAW> s_ad;
    static std::vector<glm::mat4> s_amatBone;
    s_ad.clear();
    s_amatBone.clear();
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.pglobset || sgs.irtgt >= 0 || sgs.fForeground) continue;
        const GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            if (i >= sgs.afVisible.size() || !sgs.afVisible[i] || i >= sgs.amatGlob.size() || !FCharacterGlob(gs.aglob[i])) continue;
            const GLOB& glob = gs.aglob[i];
            const GLOB* pgeom = !glob.pgl && glob.iglobTemplate >= 0 && size_t(glob.iglobTemplate) < gs.aglob.size() ? &gs.aglob[size_t(glob.iglobTemplate)] : &glob;
            if (pgeom->meshk != MESHK_Strips || !pgeom->pgl) continue;
            const GLGLOB* pgl = i < sgs.apglInst.size() && sgs.apglInst[i] && sgs.apglInst[i]->vao ? sgs.apglInst[i] : pgeom->pgl;
            uint32_t iboneFirst = 0, cbone = 0;
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty())
            {
                iboneFirst = uint32_t(s_amatBone.size());
                cbone = uint32_t(sgs.aamatBone[i].size());
                s_amatBone.insert(s_amatBone.end(), sgs.aamatBone[i].begin(), sgs.aamatBone[i].end());
            }
            for (const SUBGLOB& sub : pgeom->asubglob)
                if (sub.pshd && sub.indexCount && sub.pshd->shdk != 2 && sub.pshd->shdk != 3)
                    s_ad.push_back({pgl->vao, pgl->baseVertex, pgl->firstIndex + sub.firstIndex, sub.indexCount, &sgs.amatGlob[i], iboneFirst, cbone});
        }
    }
    SMSAVE save;
    // the frame's depth, then the characters drawn where they are the frame's surface (equal depths, a little nearer)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShDS);
    glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboCharMask);
    glViewport(0, 0, dx, dy);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!s_ad.empty())
    {
        glDisable(GL_BLEND);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_GEQUAL);   // (reversed Z)
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 64.0f);
        s_shCharMask.Use();
        glUniformMatrix4fv(s_shCharMask.Loc("matWorldToLight"), 1, GL_FALSE, glm::value_ptr(matWorldToClip));
        // The mask's triangles are the model's own. The frame drew the characters smoothed (glob.geom's PN triangles,
        // gl.h g_cSmoothGlob), their surface bulging up to a few units out of those triangles, where the mask's
        // depth test failed and the shadows and the AO fell on them in patches. So the mask is drawn that much toward
        // the camera (a little, unsmoothed, for the GS's rounding); a wall closer in front of a character than that
        // is rare and only loses its shadow there.
        glUniform3f(s_shCharMask.Loc("posEye"), posEye.x, posEye.y, posEye.z);
        glUniform1f(s_shCharMask.Loc("sTowardEye"), g_cSmoothGlob > 0 ? 6.0f : 1.5f);
        const GLint locModel = s_shCharMask.Loc("matModel"), locCBone = s_shCharMask.Loc("cbone"), locBoneFirst = s_shCharMask.Loc("iboneFirst");
        GLint ssboPrev = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 7, &ssboPrev);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssboCharMask);
        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(std::max<size_t>(1, s_amatBone.size()) * sizeof(glm::mat4)),
                     s_amatBone.empty() ? nullptr : s_amatBone.data(), GL_STREAM_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, s_ssboCharMask);
        GLuint vaoCur = 0;
        for (const MDRAW& d : s_ad)
        {
            if (d.vao != vaoCur) glBindVertexArray(vaoCur = d.vao);
            glUniform1i(locCBone, GLint(d.cbone));
            glUniform1i(locBoneFirst, GLint(d.iboneFirst));
            glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(*d.pmat));
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(d.count), GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(d.firstIndex) * 4), d.baseVertex);
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, GLuint(ssboPrev));
    }
    return true;
}

// ------------------------------------------------------------ PC: the see-through surfaces' cover
// The depth-based outlines (post.cpp, outline.frag) see only what wrote the depth: the see-through surfaces (glass,
// water, the clouds and the shafts of light: alpha-blended, their fragments drawn without the depth, as the GS's AFAIL
// FB_ONLY) let the lines of what is behind them through. Their cover is drawn again after the frame pass into an R8
// mask from the camera: the blended parts (GS ALPHA (Cs - Cd) As + Cd, or with a fixed alpha) of the visible groups,
// with their texture's and colour's alpha, where they are in front of the frame's depth (GREATER against a copy of it,
// a little farther: a surface that wrote the depth is not in front of itself), the greatest kept (MAX). The outlines
// fade under it. Built when the outlines are on (RenderScene).
GLSHADER s_shTransMask;
int s_stateTransMask = 0;   // 0 not yet, 1 built, -1 failed (no mask: the lines as before)
GLuint s_fboTransMask = 0, s_texTransMask = 0, s_ssboTransMask = 0, s_texDSTrans = 0;
int s_dxTransMask = 0, s_dyTransMask = 0;
bool s_fTransMaskFrame = false;

bool FEnsureTranslucentMask(int dx, int dy)
{
    if (s_stateTransMask == 0)
    {
        s_stateTransMask = -1;
        if (s_shTransMask.FInit("tmask.vert", "tmask.frag", "")) s_stateTransMask = 1, glGenBuffers(1, &s_ssboTransMask);
        std::printf("see-through cover: %s\n", s_stateTransMask > 0 ? "built" : "did not build: the outlines show through see-through surfaces");
    }
    if (s_stateTransMask < 0 || !FEnsureDsCopy(dx, dy)) return false;
    if (s_fboTransMask && dx == s_dxTransMask && dy == s_dyTransMask && s_texDSTrans == s_texShDS) return true;
    if (s_fboTransMask) glDeleteFramebuffers(1, &s_fboTransMask), s_fboTransMask = 0;
    if (s_texTransMask) glDeleteTextures(1, &s_texTransMask), s_texTransMask = 0;
    GLint activePrev = 0, texPrev = 0, fboPrev = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activePrev);
    glActiveTexture(GL_TEXTURE5);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texPrev);
    glGenTextures(1, &s_texTransMask);
    glBindTexture(GL_TEXTURE_2D, s_texTransMask);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, dx, dy);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, GLuint(texPrev));
    glActiveTexture(GLenum(activePrev));
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboPrev);
    glGenFramebuffers(1, &s_fboTransMask);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboTransMask);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_texTransMask, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_texShDS, 0);
    const GLenum buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buf);
    const bool fOk = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fboPrev));
    if (!fOk)
    {
        std::printf("see-through cover: its framebuffer is not usable here\n");
        s_stateTransMask = -1;
        return false;
    }
    s_dxTransMask = dx;
    s_dyTransMask = dy;
    s_texDSTrans = s_texShDS;
    return true;
}

// a pass's cover factor when it is see-through: 1 for (Cs - Cd) As + Cd, the fixed alpha / 0x80 for (Cs - Cd) FIX + Cd;
// negative when it is not (opaque, added, subtracted)
float RTranslucentPass(const SHDPASS& ps)
{
    if (!ps.fBlend) return -1.0f;
    const int a = int(ps.alpha & 3), b = int((ps.alpha >> 2) & 3), c = int((ps.alpha >> 4) & 3), d = int((ps.alpha >> 6) & 3);
    if (a != 0 || b != 1 || d != 1) return -1.0f;
    if (c == 0) return 1.0f;
    if (c == 2) return std::min(float((ps.alpha >> 32) & 0xFF) / 128.0f, 1.0f);
    return -1.0f;
}

const GLOB* PglobGeometry(const GLOBSET& gs, size_t i);

bool FBuildTranslucentMask(const SCENE& scene, const glm::mat4& matWorldToClip, GLuint fbo, int dx, int dy)
{
    if (!FEnsureTranslucentMask(dx, dy)) return false;
    struct TDRAW { GLuint vao; int32_t baseVertex; uint32_t firstIndex, count; const glm::mat4* pmat; uint32_t iboneFirst, cbone;
                   GLuint tex; uint8_t sampler, fTextured, icolor; float aConst, rAlpha; };
    static std::vector<TDRAW> s_ad;
    static std::vector<glm::mat4> s_amatBone;
    s_ad.clear();
    s_amatBone.clear();
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.pglobset || sgs.irtgt >= 0 || sgs.fForeground || sgs.gAlpha <= 0.0f) continue;
        const GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            if (i >= sgs.afVisible.size() || !sgs.afVisible[i] || i >= sgs.amatGlob.size()) continue;
            const GLOB* pgeom = PglobGeometry(gs, i);
            if (!pgeom) continue;
            const GLGLOB* pgl = i < sgs.apglInst.size() && sgs.apglInst[i] && sgs.apglInst[i]->vao ? sgs.apglInst[i] : pgeom->pgl;
            uint32_t iboneFirst = 0, cbone = 0;
            bool fBones = false;
            for (const SUBGLOB& sub : pgeom->asubglob)
            {
                if (!sub.pshd || !sub.indexCount || sub.pshd->shdk == 2 || sub.pshd->shdk == 3) continue;
                for (size_t ip = 0; ip < sub.pshd->apass.size(); ++ip)
                {
                    const SHDPASS& ps = sub.pshd->apass[ip];
                    const float r = RTranslucentPass(ps);
                    if (r <= 0.0f) continue;
                    if (!fBones && i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty())
                    {
                        iboneFirst = uint32_t(s_amatBone.size());
                        cbone = uint32_t(sgs.aamatBone[i].size());
                        s_amatBone.insert(s_amatBone.end(), sgs.aamatBone[i].begin(), sgs.aamatBone[i].end());
                    }
                    fBones = true;
                    TDRAW d{pgl->vao, pgl->baseVertex, pgl->firstIndex + sub.firstIndex, sub.indexCount, &sgs.amatGlob[i], iboneFirst, cbone,
                            0, 0, uint8_t(ps.fTextured ? 1 : 0), uint8_t(ps.icolor), float((ps.rgbaConst >> 24) & 0xFF), r * std::min(sgs.gAlpha, 1.0f)};
                    if (ps.fTextured)
                    {
                        const TEXSEL sel = TexselFromShd(sub.pshd, sub.psaa ? sub.psaa->iframe : 0, ps.itexPass);
                        d.tex = TexFromSel(sel);
                        d.sampler = SamplerFor(d.tex, uint8_t((sel.fClampS ? 1 : 0) | (sel.fClampT ? 2 : 0) | (ps.fNearest ? 4 : 0)));
                        if (!d.tex) continue;
                    }
                    s_ad.push_back(d);
                    break;   // (one pass a part: its first see-through one)
                }
            }
        }
    }
    SMSAVE save;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShDS);
    glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboTransMask);
    glViewport(0, 0, dx, dy);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!s_ad.empty())
    {
        glEnable(GL_BLEND);
        glBlendEquation(GL_MAX);
        glBlendFunc(GL_ONE, GL_ONE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_GREATER);   // (reversed Z: in front of the frame's surface)
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -64.0f);   // (a little farther: not in front of itself)
        s_shTransMask.Use();
        glUniformMatrix4fv(s_shTransMask.Loc("matWorldToLight"), 1, GL_FALSE, glm::value_ptr(matWorldToClip));
        const GLint locModel = s_shTransMask.Loc("matModel"), locCBone = s_shTransMask.Loc("cbone"), locBoneFirst = s_shTransMask.Loc("iboneFirst"),
                    locTextured = s_shTransMask.Loc("fTextured"), locIcolor = s_shTransMask.Loc("icolor"), locAConst = s_shTransMask.Loc("aConst"),
                    locRAlpha = s_shTransMask.Loc("rAlpha");
        glUniform1i(s_shTransMask.Loc("tex"), 0);
        GLint ssboPrev = 0, texPrev = 0, samplerPrev = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 7, &ssboPrev);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texPrev);
        glGetIntegerv(GL_SAMPLER_BINDING, &samplerPrev);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssboTransMask);
        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(std::max<size_t>(1, s_amatBone.size()) * sizeof(glm::mat4)),
                     s_amatBone.empty() ? nullptr : s_amatBone.data(), GL_STREAM_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, s_ssboTransMask);
        GLuint vaoCur = 0;
        for (const TDRAW& d : s_ad)
        {
            if (d.vao != vaoCur) glBindVertexArray(vaoCur = d.vao);
            if (d.fTextured)
            {
                glBindTexture(GL_TEXTURE_2D, d.tex);
                glBindSampler(0, s_asampler[d.sampler & 15u]);
            }
            glUniform1i(locTextured, d.fTextured);
            glUniform1i(locIcolor, d.icolor);
            glUniform1f(locAConst, d.aConst);
            glUniform1f(locRAlpha, d.rAlpha);
            glUniform1i(locCBone, GLint(d.cbone));
            glUniform1i(locBoneFirst, GLint(d.iboneFirst));
            glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(*d.pmat));
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(d.count), GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(d.firstIndex) * 4), d.baseVertex);
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
        glBlendEquation(GL_FUNC_ADD);
        glBindTexture(GL_TEXTURE_2D, GLuint(texPrev));
        glBindSampler(0, GLuint(samplerPrev));
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, GLuint(ssboPrev));
    }
    // (the draws kept out of the outlines, the job markers: full cover where they are the frame's surface)
    if (!s_aextraNoOutline.empty())
    {
        glDisable(GL_BLEND);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_GEQUAL);   // (reversed Z: where they are the surface, a little nearer)
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 64.0f);
        s_shTransMask.Use();
        glUniformMatrix4fv(s_shTransMask.Loc("matWorldToLight"), 1, GL_FALSE, glm::value_ptr(matWorldToClip));
        glUniform1i(s_shTransMask.Loc("fTextured"), 0);
        glUniform1i(s_shTransMask.Loc("icolor"), 2);
        glUniform1f(s_shTransMask.Loc("aConst"), 128.0f);
        glUniform1f(s_shTransMask.Loc("rAlpha"), 1.0f);
        glUniform1i(s_shTransMask.Loc("cbone"), 0);
        const GLint locModel = s_shTransMask.Loc("matModel");
        GLuint vaoCur = 0;
        for (const EXTRADRAW& x : s_aextraNoOutline)
        {
            if (!x.psgs || !x.psgs->pglobset) continue;
            const GLOBSET& gs = *x.psgs->pglobset;
            if (x.iglob >= gs.aglob.size()) continue;
            const GLOB* pgeom = PglobGeometry(gs, x.iglob);
            if (!pgeom) continue;
            if (pgeom->pgl->vao != vaoCur) glBindVertexArray(vaoCur = pgeom->pgl->vao);
            glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(x.mat));
            for (const SUBGLOB& sub : pgeom->asubglob)
                if (sub.indexCount)
                    glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(sub.indexCount), GL_UNSIGNED_INT,
                                             reinterpret_cast<void*>(size_t(pgeom->pgl->firstIndex + sub.firstIndex) * 4), pgeom->pgl->baseVertex);
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    static size_t s_cPrinted = size_t(-1);
    if (std::getenv("SLY2N_WSPRINT") && s_ad.size() != s_cPrinted) std::printf("see-through cover: %zu parts\n", s_cPrinted = s_ad.size());
    return true;
}

bool FWorldShadowsReady()
{
    if (s_stateWs != 0) return s_stateWs > 0;
    s_stateWs = -1;
    if (s_shWsDepth.FInit("smdepth.vert", "smdepth.frag", "") && s_shWsApply.FInit("shadowcombine.vert", "wsapply.frag", ""))
    {
        GLint texPrev = 0, fboPrev = 0, activePrev = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activePrev);
        glActiveTexture(GL_TEXTURE4);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texPrev);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboPrev);
        bool fOk = true;
        for (int k = 0; k < 2; ++k)
        {
            GLuint& tex = k == 0 ? s_texWs : s_texWsStatic;
            GLuint& fboMap = k == 0 ? s_fboWs : s_fboWsStatic;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, kcWsMap, kcWsMap);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
            glGenFramebuffers(1, &fboMap);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboMap);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
            glDrawBuffer(GL_NONE);
            fOk = fOk && glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        glBindTexture(GL_TEXTURE_2D, GLuint(texPrev));
        glActiveTexture(GLenum(activePrev));
        if (fOk) s_stateWs = 1;
        glGenBuffers(1, &s_ssboWs);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fboPrev));
    }
    std::printf("world shadows (TEST): %s\n", s_stateWs > 0 ? "on" : "did not build: off");
    return s_stateWs > 0;
}

// The light the world's shadows fall from (its direction, from the light), printed whenever it changes.
glm::vec3 DirWorldShadowLight()
{
    static uint32_t s_version = ~0u;
    static glm::vec3 s_dir(0.0f, 0.0f, -1.0f);
    const LIGHTSET& lights = WorldLights();
    if (lights.alight.empty() && lights.actx.empty() && s_version != ~0u) return s_dir;
    if (WorldLightsVersion() == s_version) return s_dir;
    s_version = WorldLightsVersion();
    const char* pszFrom = "the default (high above)";
    glm::vec3 dir(0.25f, 0.18f, -1.0f);
    if (const char* psz = std::getenv("SLY2N_WORLDSHADOWDIR"))
    {
        glm::vec3 v(0.0f);
        if (std::sscanf(psz, "%f%*[ ,]%f%*[ ,]%f", &v.x, &v.y, &v.z) == 3 && glm::length(v) > 1e-3f) dir = v, pszFrom = "SLY2N_WORLDSHADOWDIR";
    }
    else
    {
        float gBest = 0.0f;
        auto consider = [&](const LIGHTREC& r, bool fShadow, const char* psz) {
            if (r.kind != 0 || !fShadow || glm::length(r.dirOrPos) < 1e-3f) return;
            const float g = r.rgba.r + r.rgba.g + r.rgba.b;
            if (g > gBest) gBest = g, dir = r.dirOrPos, pszFrom = psz;
        };
        for (const auto& pr : lights.actx) consider(pr.first, pr.second.fShadow != 0, "the level's directional shadow light");
        if (gBest <= 0.0f)
            for (const LIGHTREC& r : lights.alight) consider(r, true, "the level's strongest directional light");
    }
    dir = glm::normalize(dir);
    if (dir.z > -0.2f) dir = glm::normalize(glm::vec3(0.25f, 0.18f, -1.0f)), pszFrom = "the default (the level's light was too low)";
    // (no lower than about 25 degrees over the horizon: the shadows at most about twice as long as the caster is high)
    if (dir.z > -0.42f)
    {
        const glm::vec2 h = glm::normalize(glm::vec2(dir.x, dir.y)) * std::sqrt(1.0f - 0.42f * 0.42f);
        dir = glm::vec3(h, -0.42f);
    }
    s_dir = dir;
    // (the lights' version changes whenever any light moves or flickers, often every step: printed only when the
    // direction itself changes)
    static glm::vec3 s_dirPrinted(0.0f);
    if (glm::length(dir - s_dirPrinted) > 0.02f)
    {
        s_dirPrinted = dir;
        std::printf("world shadows (TEST): the light from %s, direction %.2f %.2f %.2f\n", pszFrom, dir.x, dir.y, dir.z);
    }
    return s_dir;
}

// The box the level's shadows are drawn in: round the point ahead of the camera, its centre snapped to the map's texels
// in the light's frame (no shimmer as the camera moves).
WSBOX BoxWorldShadows(const glm::vec3& posCamera, const glm::vec3& dirCamera)
{
    WSBOX box;
    box.L = DirWorldShadowLight();
    const glm::vec3 L = box.L;
    const glm::vec3 up = std::fabs(L.z) < 0.95f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 matRot = glm::lookAt(glm::vec3(0.0f), L, up);
    const glm::vec3 posFocus = posCamera + glm::normalize(glm::vec3(dirCamera.x, dirCamera.y, 0.0f) + glm::vec3(1e-4f, 0.0f, 0.0f)) * (kcWsRadius * 0.45f);
    glm::vec3 c = glm::vec3(matRot * glm::vec4(posFocus, 1.0f));
    box.texel = 2.0f * kcWsRadius / float(kcWsMap);
    // (snapped to 64 texels, about 80 units: the box stays still while the camera moves within that, and the map of
    // the casters that stay still is drawn again only when it moves; whole texels, so no shimmer)
    const float snap = box.texel * 64.0f;
    c.x = std::floor(c.x / snap) * snap;
    c.y = std::floor(c.y / snap) * snap;
    const glm::mat4 matView = glm::translate(glm::mat4(1.0f), glm::vec3(-c.x, -c.y, -c.z - kcWsDepth * 0.5f)) * matRot;
    box.matLight = glm::orthoRH_ZO(-kcWsRadius, kcWsRadius, -kcWsRadius, kcWsRadius, 1.0f, kcWsDepth) * matView;
    box.cWorld = glm::vec3(glm::inverse(matRot) * glm::vec4(c, 1.0f));
    return box;
}

// Why a whole object casts no level shadow (WSWHY_Casts: its groups are looked at one by one)
WSWHY WsWhySet(const SCENEGLOBSET& sgs, const WSBOX& box)
{
    // (the characters: their own shadows. An actor's tree that has a character's groups, the FBA kinds: Sly, the gang,
    // a guard, their canes and hats with them; not every actor's, the lanterns on their wires, the swinging signs and
    // the other moving props are actors too, and cast)
    if (sgs.fActorTree)
    {
        const GLOBSET& gs = *sgs.pglobset;
        for (const GLOB& glob : gs.aglob)
            if (FCharacterGlob(glob)) return WSWHY_Character;
    }
    if (sgs.gAlpha < 0.5f) return WSWHY_Faded;
    // (an object wider than the sky's radius is looked at group by group, WsWhyGlob: the sky, but also a world whose
    // whole level is one object, Paris's, oid 1, about 43000 wide)
    if (sgs.fBounds && sgs.sBoundsRadius <= kcWsSkyRadius)
    {
        // (out of the box across the light: no shadow in it)
        const glm::vec3 d = sgs.posBounds - box.cWorld;
        const glm::vec3 dAcross = d - box.L * glm::dot(d, box.L);
        if (glm::length(dAcross) > kcWsRadius * 1.42f + sgs.sBoundsRadius) return WSWHY_Out;
    }
    return WSWHY_Casts;
}

// A group's sphere in the space its triangles are drawn from (the vertices' own, under its matrix): its vertices'
// bounds, found once. The records' spheres (the draw record's, the sphere sets') are not in that space for the level's
// own groups (they called groups beside the camera out of the box), so they are not used; no vertices: unknown.
glm::vec4 SphereOfGlob(const GLOB& geom)
{
    // (kept with the vertices it was found from: a later world's group at the same address finds its own)
    struct SPHEREREC { glm::vec4 sphere; const VTXGLOB* pvtx; size_t cvtx; };
    static std::unordered_map<const GLOB*, SPHEREREC> s_mpsphere;
    if (geom.avtx.empty()) return glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
    if (s_mpsphere.size() > 200000) s_mpsphere.clear();
    SPHEREREC& rec = s_mpsphere[&geom];
    if (rec.pvtx == geom.avtx.data() && rec.cvtx == geom.avtx.size()) return rec.sphere;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (const VTXGLOB& v : geom.avtx) lo = glm::min(lo, v.pos), hi = glm::max(hi, v.pos);
    rec = {glm::vec4((lo + hi) * 0.5f, glm::length(hi - lo) * 0.5f), geom.avtx.data(), geom.avtx.size()};
    return rec.sphere;
}

// Why a group casts no level shadow: a character's (the FBA kinds); and in an object too wide to be judged whole
// (WsWhySet: the sky's, or a world whose whole level is one object, as Paris's oid 1, about 43000 wide) a sky-wide
// group, or one out of the box
WSWHY WsWhyGlob(const SCENEGLOBSET& sgs, const GLOB& glob, const GLOB& geom, const glm::mat4& mat, const WSBOX& box)
{
    if (FCharacterGlob(glob)) return WSWHY_Character;
    if (!sgs.fBounds || sgs.sBoundsRadius <= kcWsSkyRadius) return WSWHY_Casts;
    const glm::vec4 sphere = SphereOfGlob(geom);
    if (sphere.w < 0.0f) return WSWHY_Casts;
    const float rScale = std::max({glm::length(glm::vec3(mat[0])), glm::length(glm::vec3(mat[1])), glm::length(glm::vec3(mat[2]))});
    const float r = sphere.w * rScale;
    if (r > kcWsSkyGlobRadius) return WSWHY_Sky;
    const glm::vec3 d = glm::vec3(mat * glm::vec4(glm::vec3(sphere), 1.0f)) - box.cWorld;
    const glm::vec3 dAcross = d - box.L * glm::dot(d, box.L);
    if (glm::length(dAcross) > kcWsRadius * 1.42f + r) return WSWHY_Out;
    return WSWHY_Casts;
}

// A group's triangles (an instance's are its template's), null when it has none to draw (not strips)
const GLOB* PglobGeometry(const GLOBSET& gs, size_t i)
{
    const GLOB& glob = gs.aglob[i];
    const GLOB* pgeom = &glob;
    if (!glob.pgl && glob.iglobTemplate >= 0 && size_t(glob.iglobTemplate) < gs.aglob.size()) pgeom = &gs.aglob[size_t(glob.iglobTemplate)];
    return pgeom->meshk == MESHK_Strips && pgeom->pgl ? pgeom : nullptr;
}

// Why a part of a group casts no level shadow: no shader or triangles, the volumes of light (kind 3), the PS2's own
// shadow shapes (kind 2), the additive glows
WSWHY WsWhySub(const SUBGLOB& sub)
{
    if (!sub.pshd || sub.indexCount == 0 || sub.pshd->apass.empty()) return WSWHY_Empty;
    if (sub.pshd->shdk == 3) return WSWHY_LightVolume;
    if (sub.pshd->shdk == 2) return WSWHY_Ps2Shadow;
    const SHDPASS& ps = sub.pshd->apass[0];
    if (ps.fBlend && ((ps.alpha >> 2) & 3) == 2 && ((ps.alpha >> 6) & 3) == 1) return WSWHY_Glow;
    return WSWHY_Casts;
}

// The level's shadows' strength (0..100) in the current episode: its own setting, else the global one.
int NWorldShadowsNow()
{
    const int iep = IepisodeCur();
    if (iep >= 0 && iep < 9 && g_display.aWorldShadowsEpisode[iep] >= 0) return std::min(g_display.aWorldShadowsEpisode[iep], 100);
    return std::clamp(g_display.worldShadows, 0, 100);
}

// ------------------------------------------------------------ PC: lamp shadows (TEST, display.h lampShadows)
// The level's point lights (the bake's lights of kind 1: their light reaches as far as distanceOffset + distance x
// distanceScale stays above 0) cast shadows too, while the level's shadows are on: each frame the few that light the
// most of what is round the camera (bright, near, wide; one chosen last frame kept unless another is clearly better,
// and a new one faded in) each get a cube map of the casters within their reach (DrawWorldShadows' casters: not the
// characters, the glows, the sky), its six faces in one pass (lsdepth.geom, layered), the distance over the reach as
// the depth (lsdepth.frag); then every pixel looked up in them (lsapply.frag), the shadow as dark as the lamp lights it.
constexpr int kcLampMax = 4, kcLampFace = 512;
constexpr float kcLampRangeMin = 60.0f, kcLampRangeMax = 6000.0f;
GLSHADER s_shLsDepth, s_shLsApply;
int s_stateLs = 0;
GLuint s_fboLs = 0, s_texLs = 0, s_ssboLs = 0;
struct LAMP { glm::vec3 pos{0.0f}; float range = 0.0f, off = 0.0f, scale = 0.0f, gWeight = 0.0f; int islot = 0; };
// The cube maps' slots: a lamp keeps its slot while it is chosen, and its map is drawn again only when what it was drawn
// from changed (its casters' draws and matrices; a skinned caster in reach: each time), at most one such a frame
// (round the slots in turn: the others a few frames late); a lamp new to its slot is drawn at once. Each slot's six
// layers have their own framebuffer (a view of them), cleared alone.
struct LAMPSLOT { bool fUsed = false; glm::vec3 pos{0.0f}; uint64_t key = 0; GLuint texView = 0, fbo = 0; };
LAMPSLOT s_aslotLamp[kcLampMax];
int s_islotLampNext = 0;
int s_cLampMapsDrawn = 0;   // (SLY2N_WSPRINT)
LAMP s_alampPrev[kcLampMax];
int s_clampPrev = 0;

bool FLampShadowsReady()
{
    if (s_stateLs != 0) return s_stateLs > 0;
    s_stateLs = -1;
    if (s_shLsDepth.FInit("lsdepth.vert", "lsdepth.geom", "lsdepth.frag", "") && s_shLsApply.FInit("shadowcombine.vert", "lsapply.frag", ""))
    {
        GLint texPrev = 0, fboPrev = 0, activePrev = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activePrev);
        glActiveTexture(GL_TEXTURE4);
        glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP_ARRAY, &texPrev);
        glGenTextures(1, &s_texLs);
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, s_texLs);
        glTexStorage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 1, GL_DEPTH_COMPONENT32F, kcLampFace, kcLampFace, 6 * kcLampMax);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, GLuint(texPrev));
        glActiveTexture(GLenum(activePrev));
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboPrev);
        glGenFramebuffers(1, &s_fboLs);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboLs);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, s_texLs, 0);   // (layered: gl_Layer picks the face)
        glDrawBuffer(GL_NONE);
        bool fOk = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        for (LAMPSLOT& slot : s_aslotLamp)
        {
            const int islot = int(&slot - s_aslotLamp);
            glGenTextures(1, &slot.texView);
            glTextureView(slot.texView, GL_TEXTURE_2D_ARRAY, s_texLs, GL_DEPTH_COMPONENT32F, 0, 1, GLuint(6 * islot), 6);
            glGenFramebuffers(1, &slot.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, slot.fbo);
            glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, slot.texView, 0);   // (its six layers: 0..5)
            glDrawBuffer(GL_NONE);
            fOk = fOk && glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        if (fOk) s_stateLs = 1;
        glGenBuffers(1, &s_ssboLs);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fboPrev));
    }
    std::printf("lamp shadows (TEST): %s\n", s_stateLs > 0 ? "on" : "did not build: off");
    return s_stateLs > 0;
}

int NLampShadowsNow()
{
    static int s_n = -2;
    if (s_n == -2)
    {
        s_n = -1;
        if (const char* psz = std::getenv("SLY2N_LAMPSHADOWS")) s_n = std::clamp(std::atoi(psz), 0, kcLampMax);
    }
    return s_n >= 0 ? s_n : std::clamp(g_display.lampShadows, 0, kcLampMax);
}

// The lamps of this frame: the level's point lights with a reach, scored by how much of what is round the camera they
// light (their brightness, x the share of the reach left at the camera's focus); last frame's kept by a margin
int CLampsChosen(const glm::vec3& posFocus, int clampMax, LAMP alamp[kcLampMax])
{
    struct CAND { LAMP lamp; float score; };
    static std::vector<CAND> s_acand;
    s_acand.clear();
    for (const LIGHTREC& r : WorldLights().alight)
    {
        if (r.kind != 1 || r.distanceScale >= 0.0f || r.distanceOffset <= 0.0f) continue;
        const float range = -r.distanceOffset / r.distanceScale;
        if (range < kcLampRangeMin || range > kcLampRangeMax) continue;   // (too small to see, or a whole area's fill)
        const float gBright = std::max({r.rgba.r, r.rgba.g, r.rgba.b});
        if (gBright <= 0.0f) continue;
        const float d = glm::distance(posFocus, r.dirOrPos);
        if (d > range + 1800.0f) continue;   // (its light out of sight round the camera)
        float score = gBright * std::sqrt(range) * (1.0f - d / (range + 1800.0f));
        for (int i = 0; i < s_clampPrev; ++i)
            if (glm::distance(s_alampPrev[i].pos, r.dirOrPos) < 1.0f) score *= 1.35f;   // (kept unless clearly beaten)
        s_acand.push_back({LAMP{r.dirOrPos, range, r.distanceOffset, r.distanceScale, 0.0f}, score});
    }
    const size_t c = std::min<size_t>(size_t(clampMax), s_acand.size());
    std::partial_sort(s_acand.begin(), s_acand.begin() + ptrdiff_t(c), s_acand.end(), [](const CAND& a, const CAND& b) { return a.score > b.score; });
    for (size_t i = 0; i < c; ++i)
    {
        alamp[i] = s_acand[i].lamp;
        // (faded in over about a third of a second when newly chosen)
        float gPrev = 0.0f;
        for (int j = 0; j < s_clampPrev; ++j)
            if (glm::distance(s_alampPrev[j].pos, alamp[i].pos) < 1.0f) gPrev = s_alampPrev[j].gWeight;
        alamp[i].gWeight = std::min(1.0f, gPrev + 0.05f);
    }
    s_clampPrev = int(c);
    for (size_t i = 0; i < c; ++i) s_alampPrev[i] = alamp[i];
    return int(c);
}

// this frame's lamps (CBuildLampMaps), for the level's shadows (wsapply.frag: where a lamp lights a surface, the
// moon's shadow fades) and their own (ApplyLampShadows)
LAMP s_alampFrame[kcLampMax];
int s_clampFrame = 0;

// The lamps' cube maps (0: no lamps this frame)
int CBuildLampMaps(const SCENE& scene, const glm::vec3& posCamera, const glm::vec3& dirCamera)
{
    s_clampFrame = 0;
    if (!FLampShadowsReady()) return 0;
    LAMP* alamp = s_alampFrame;
    const glm::vec3 posFocus = posCamera + glm::normalize(dirCamera + glm::vec3(1e-4f, 0.0f, 0.0f)) * 500.0f;
    const int clamp = CLampsChosen(posFocus, NLampShadowsNow(), alamp);
    // their slots: kept, else a free one (drawn at once)
    bool afTaken[kcLampMax] = {};
    for (int l = 0; l < clamp; ++l)
    {
        alamp[l].islot = -1;
        for (int k = 0; k < kcLampMax; ++k)
            if (!afTaken[k] && s_aslotLamp[k].fUsed && glm::distance(s_aslotLamp[k].pos, alamp[l].pos) < 1.0f) { alamp[l].islot = k; afTaken[k] = true; break; }
    }
    for (int l = 0; l < clamp; ++l)
        if (alamp[l].islot < 0)
            for (int k = 0; k < kcLampMax; ++k)
                if (!afTaken[k])
                {
                    alamp[l].islot = k;
                    afTaken[k] = true;
                    s_aslotLamp[k].pos = alamp[l].pos;
                    s_aslotLamp[k].key = 0;   // (new here: drawn at once)
                    break;
                }
    for (int k = 0; k < kcLampMax; ++k) s_aslotLamp[k].fUsed = afTaken[k];
    static int s_clampPrinted = -1;
    if (clamp != s_clampPrinted && std::getenv("SLY2N_WSPRINT"))
    {
        s_clampPrinted = clamp;
        std::printf("lamp shadows: %d lamps", clamp);
        for (int i = 0; i < clamp; ++i) std::printf("  (%.0f %.0f %.0f reach %.0f)", alamp[i].pos.x, alamp[i].pos.y, alamp[i].pos.z, alamp[i].range);
        std::printf("\n");
    }
    if (clamp == 0) return 0;

    // the casters, with their world spheres (DrawWorldShadows' sorting, without its box)
    struct LSDRAW { GLuint vao; int32_t baseVertex; uint32_t firstIndex, count; const glm::mat4* pmat; uint32_t iboneFirst, cbone; glm::vec4 sphere; };
    static std::vector<LSDRAW> s_ad;
    static std::vector<glm::mat4> s_amatBoneLs;
    s_ad.clear();
    s_amatBoneLs.clear();
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.pglobset || sgs.irtgt >= 0 || sgs.fForeground) continue;
        if (sgs.fActorTree)
        {
            bool fChar = false;
            for (const GLOB& glob : sgs.pglobset->aglob) fChar = fChar || FCharacterGlob(glob);
            if (fChar) continue;
        }
        if (sgs.gAlpha < 0.5f) continue;
        const bool fWhole = sgs.fBounds && sgs.sBoundsRadius <= kcWsSkyRadius;
        if (fWhole)
        {
            bool fNear = false;
            for (int l = 0; l < clamp && !fNear; ++l) fNear = glm::distance(sgs.posBounds, alamp[l].pos) < alamp[l].range + sgs.sBoundsRadius;
            if (!fNear) continue;
        }
        const GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            if (i >= sgs.afVisible.size() || !sgs.afVisible[i] || i >= sgs.amatGlob.size()) continue;
            const GLOB& glob = gs.aglob[i];
            if (FCharacterGlob(glob)) continue;
            const GLOB* pgeom = PglobGeometry(gs, i);
            if (!pgeom) continue;
            const glm::mat4& mat = sgs.amatGlob[i];
            glm::vec4 sphere(sgs.posBounds, sgs.sBoundsRadius);
            if (!fWhole)
            {
                const glm::vec4 s = SphereOfGlob(*pgeom);
                if (s.w < 0.0f) continue;   // (unknown, in an object too wide to judge whole: left out)
                const float rScale = std::max({glm::length(glm::vec3(mat[0])), glm::length(glm::vec3(mat[1])), glm::length(glm::vec3(mat[2]))});
                sphere = glm::vec4(glm::vec3(mat * glm::vec4(glm::vec3(s), 1.0f)), s.w * rScale);
                if (sphere.w > kcWsSkyGlobRadius) continue;
                bool fNear = false;
                for (int l = 0; l < clamp && !fNear; ++l) fNear = glm::distance(glm::vec3(sphere), alamp[l].pos) < alamp[l].range + sphere.w;
                if (!fNear) continue;
            }
            const GLGLOB* pgl = i < sgs.apglInst.size() && sgs.apglInst[i] && sgs.apglInst[i]->vao ? sgs.apglInst[i] : pgeom->pgl;
            uint32_t iboneFirst = 0, cbone = 0;
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty())
            {
                iboneFirst = uint32_t(s_amatBoneLs.size());
                cbone = uint32_t(sgs.aamatBone[i].size());
                s_amatBoneLs.insert(s_amatBoneLs.end(), sgs.aamatBone[i].begin(), sgs.aamatBone[i].end());
            }
            for (const SUBGLOB& sub : pgeom->asubglob)
                if (WsWhySub(sub) == WSWHY_Casts)
                    s_ad.push_back({pgl->vao, pgl->baseVertex, pgl->firstIndex + sub.firstIndex, sub.indexCount, &mat, iboneFirst, cbone, sphere});
        }
    }

    // which lamps' maps to draw: new in their slot; else changed (their casters' draws and matrices), one a frame
    uint64_t akey[kcLampMax] = {};
    bool afDraw[kcLampMax] = {};
    static uint64_t s_cframeLs = 0;
    ++s_cframeLs;
    for (int l = 0; l < clamp; ++l)
    {
        uint64_t key = 1469598103934665603ull;
        auto mix = [&key](const void* pv, size_t cb) {
            const uint8_t* pb = static_cast<const uint8_t*>(pv);
            for (size_t k = 0; k < cb; ++k) key = (key ^ pb[k]) * 1099511628211ull;
        };
        mix(&alamp[l].pos, sizeof(glm::vec3));
        mix(&alamp[l].range, sizeof(float));
        for (const LSDRAW& d : s_ad)
        {
            if (glm::distance(glm::vec3(d.sphere), alamp[l].pos) >= alamp[l].range + d.sphere.w) continue;
            mix(&d.vao, sizeof(d.vao));
            mix(&d.firstIndex, sizeof(d.firstIndex));
            mix(&d.count, sizeof(d.count));
            mix(d.pmat, sizeof(glm::mat4));
            if (d.cbone > 0) mix(&s_cframeLs, sizeof(s_cframeLs));   // (skinned: its bones move; drawn in its turn)
        }
        akey[l] = key == 0 ? 1 : key;
        afDraw[l] = s_aslotLamp[alamp[l].islot].key == 0;
    }
    for (int t = 0; t < kcLampMax; ++t)
    {
        const int k = (s_islotLampNext + t) % kcLampMax;
        const int l = int(std::find_if(alamp, alamp + clamp, [k](const LAMP& m) { return m.islot == k; }) - alamp);
        if (l < clamp && !afDraw[l] && akey[l] != s_aslotLamp[k].key)
        {
            afDraw[l] = true;
            s_islotLampNext = (k + 1) % kcLampMax;
            break;
        }
    }

    SMSAVE save;
    // the cube maps: each lamp's six faces drawn (one layered pass), into its slot's own layers
    glViewport(0, 0, kcLampFace, kcLampFace);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    s_shLsDepth.Use();
    const GLint locModel = s_shLsDepth.Loc("matModel"), locCBone = s_shLsDepth.Loc("cbone"), locBoneFirst = s_shLsDepth.Loc("iboneFirst");
    GLint ssboPrev = 0;
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 7, &ssboPrev);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssboLs);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(std::max<size_t>(1, s_amatBoneLs.size()) * sizeof(glm::mat4)),
                 s_amatBoneLs.empty() ? nullptr : s_amatBoneLs.data(), GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, s_ssboLs);
    // (the cube map's faces: +X -X +Y -Y +Z -Z, each looking out with the GL cube map's own up)
    static const glm::vec3 s_adir[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 s_aup[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    BeginAddedGpuTimer(AGT_LampMaps);
    for (int l = 0; l < clamp; ++l)
    {
        if (!afDraw[l]) continue;
        const LAMP& lamp = alamp[l];
        LAMPSLOT& slot = s_aslotLamp[lamp.islot];
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, slot.fbo);
        glClear(GL_DEPTH_BUFFER_BIT);
        const glm::mat4 matProj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 2.0f, lamp.range);
        glm::mat4 amat[6];
        for (int f = 0; f < 6; ++f) amat[f] = matProj * glm::lookAt(lamp.pos, lamp.pos + s_adir[f], s_aup[f]);
        glUniformMatrix4fv(s_shLsDepth.Loc("amatFace"), 6, GL_FALSE, glm::value_ptr(amat[0]));
        glUniform1i(s_shLsDepth.Loc("layerFirst"), 0);
        glUniform3f(s_shLsDepth.Loc("posLight"), lamp.pos.x, lamp.pos.y, lamp.pos.z);
        glUniform1f(s_shLsDepth.Loc("rcpRange"), 1.0f / lamp.range);
        GLuint vaoCur = 0;
        for (const LSDRAW& d : s_ad)
        {
            if (glm::distance(glm::vec3(d.sphere), lamp.pos) >= lamp.range + d.sphere.w) continue;
            if (d.vao != vaoCur) glBindVertexArray(vaoCur = d.vao);
            glUniform1i(locCBone, GLint(d.cbone));
            glUniform1i(locBoneFirst, GLint(d.iboneFirst));
            glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(*d.pmat));
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(d.count), GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(d.firstIndex) * 4), d.baseVertex);
        }
        slot.key = akey[l];
        ++s_cLampMapsDrawn;
    }
    EndAddedGpuTimer(AGT_LampMaps);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, GLuint(ssboPrev));
    s_clampFrame = clamp;
    return clamp;
}

// The lamps' uniforms (lsapply.frag's, wsapply.frag's): their places and reaches, fall-offs and weights
void SetLampUniforms(GLSHADER& sh)
{
    glm::vec4 avLamp[kcLampMax], avAtt[kcLampMax];
    for (int l = 0; l < s_clampFrame; ++l)
    {
        avLamp[l] = glm::vec4(s_alampFrame[l].pos, s_alampFrame[l].range);
        avAtt[l] = glm::vec4(s_alampFrame[l].off, s_alampFrame[l].scale, s_alampFrame[l].gWeight, float(s_alampFrame[l].islot));
    }
    glUniform1i(sh.Loc("lampCount"), s_clampFrame);
    if (s_clampFrame > 0)
    {
        glUniform4fv(sh.Loc("alamp"), s_clampFrame, glm::value_ptr(avLamp[0]));
        glUniform4fv(sh.Loc("aatt"), s_clampFrame, glm::value_ptr(avAtt[0]));
    }
    glUniform1f(sh.Loc("rcpFace"), 2.0f / float(kcLampFace));
}

// The lamps' own shadows over the frame (after the level's)
void ApplyLampShadows(const glm::mat4& matWorldToClip, GLuint fbo, int dx, int dy)
{
    if (s_clampFrame == 0 || !FEnsureDsCopy(dx, dy)) return;
    SMSAVE save;
    // the frame: its depth copied, every pixel looked up
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShDS);
    glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, dx, dy);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, s_texShDS);
    glBindSampler(3, 0);
    GLint texCube4 = 0, sampler4 = 0, texUnit5 = 0, samplerUnit5 = 0;
    glActiveTexture(GL_TEXTURE4);
    glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP_ARRAY, &texCube4);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler4);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, s_texLs);
    glBindSampler(4, 0);
    glActiveTexture(GL_TEXTURE5);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texUnit5);
    glGetIntegerv(GL_SAMPLER_BINDING, &samplerUnit5);
    glBindTexture(GL_TEXTURE_2D, s_fCharMaskFrame ? s_texCharMask : 0);
    glBindSampler(5, 0);
    glActiveTexture(GL_TEXTURE0);
    s_shLsApply.Use();
    glUniform1i(s_shLsApply.Loc("texDepth"), 3);
    glUniform1i(s_shLsApply.Loc("texLs"), 4);
    glUniform1i(s_shLsApply.Loc("texMask"), 5);
    glUniform1i(s_shLsApply.Loc("fMask"), s_fCharMaskFrame ? 1 : 0);
    const glm::mat4 matClipToWorld = glm::inverse(matWorldToClip);
    glUniformMatrix4fv(s_shLsApply.Loc("matClipToWorld"), 1, GL_FALSE, glm::value_ptr(matClipToWorld));
    glUniform2f(s_shLsApply.Loc("viewSize"), float(dx), float(dy));
    SetLampUniforms(s_shLsApply);
    glUniform1f(s_shLsApply.Loc("strength"), float(NWorldShadowsNow()) / 100.0f);
    glBindVertexArray(s_vaoEmpty);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);   // (the colour darkened; the alpha kept)
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    BeginAddedGpuTimer(AGT_LampApply);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    EndAddedGpuTimer(AGT_LampApply);
    glActiveTexture(GL_TEXTURE4);   // (unit 4's cube map array and unit 5 put back: SMSAVE keeps unit 4's 2D)
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, GLuint(texCube4));
    glBindSampler(4, GLuint(sampler4));
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, GLuint(texUnit5));
    glBindSampler(5, GLuint(samplerUnit5));
    glActiveTexture(GL_TEXTURE0);
}

void DrawWorldShadows(const SCENE& scene, const glm::mat4& matWorldToClip, const glm::vec3& posCamera, const glm::vec3& dirCamera,
                      GLuint fbo, int dx, int dy)
{
    if (!FWorldShadowsReady() || !FEnsureDsCopy(dx, dy)) return;
    const WSBOX box = BoxWorldShadows(posCamera, dirCamera);
    const glm::vec3 L = box.L;
    const glm::mat4& matLight = box.matLight;
    const float texel = box.texel;

    struct WSDRAW { GLuint vao; int32_t baseVertex; uint32_t firstIndex, count; const glm::mat4* pmat; uint32_t iboneFirst, cbone; bool fMoving; };
    static std::vector<WSDRAW> s_ad;
    static std::vector<glm::mat4> s_amatBoneWs;
    s_ad.clear();
    s_amatBoneWs.clear();
    // (SLY2N_WSPRINT: what was left out, and why)
    int cActor = 0, cSky = 0, cFaded = 0, cOut = 0, cNotStrips = 0;
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.pglobset || sgs.irtgt >= 0 || sgs.fForeground) continue;
        switch (WsWhySet(sgs, box))
        {
        case WSWHY_Character: ++cActor; continue;
        case WSWHY_Faded: ++cFaded; continue;
        case WSWHY_Sky: ++cSky; continue;
        case WSWHY_Out: ++cOut; continue;
        default: break;
        }
        const GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            if (i >= sgs.afVisible.size() || !sgs.afVisible[i] || i >= sgs.amatGlob.size()) continue;
            const GLOB& glob = gs.aglob[i];
            const GLOB* pgeom = PglobGeometry(gs, i);
            if (!pgeom) { if (FCharacterGlob(glob)) ++cActor; else ++cNotStrips; continue; }
            switch (WsWhyGlob(sgs, glob, *pgeom, sgs.amatGlob[i], box))
            {
            case WSWHY_Character: ++cActor; continue;   // (the characters' groups: their own shadows)
            case WSWHY_Sky: ++cSky; continue;
            case WSWHY_Out: ++cOut; continue;
            default: break;
            }
            const GLGLOB* pgl = i < sgs.apglInst.size() && sgs.apglInst[i] && sgs.apglInst[i]->vao ? sgs.apglInst[i] : pgeom->pgl;
            // (skinned props, an awning's or a sign's, with their bones; the characters were left out above)
            uint32_t iboneFirst = 0, cbone = 0;
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty())
            {
                iboneFirst = uint32_t(s_amatBoneWs.size());
                cbone = uint32_t(sgs.aamatBone[i].size());
                s_amatBoneWs.insert(s_amatBoneWs.end(), sgs.aamatBone[i].begin(), sgs.aamatBone[i].end());
            }
            for (const SUBGLOB& sub : pgeom->asubglob)
            {
                if (WsWhySub(sub) != WSWHY_Casts) continue;
                // (the actors' and the skinned: moving, drawn each frame; the rest kept in the still map)
                s_ad.push_back({pgl->vao, pgl->baseVertex, pgl->firstIndex + sub.firstIndex, sub.indexCount, &sgs.amatGlob[i], iboneFirst, cbone,
                                sgs.fActorTree || cbone > 0});
            }
        }
    }
    static size_t s_cPrinted = size_t(-1);
    if (std::getenv("SLY2N_WSPRINT") && s_ad.size() != s_cPrinted)
        std::printf("world shadows: %zu draws (%zu bones); left out: %d characters, %d sky-wide, %d faded, %d out of the box, %d not meshes\n",
                    s_cPrinted = s_ad.size(), s_amatBoneWs.size(), cActor, cSky, cFaded, cOut, cNotStrips);

    // the still map's key: the box, and every still caster's draw and matrix (a door or a lift that is not an actor
    // moves its matrix: drawn again)
    uint64_t key = 1469598103934665603ull;
    auto mix = [&key](const void* pv, size_t cb) {
        const uint8_t* pb = static_cast<const uint8_t*>(pv);
        for (size_t k = 0; k < cb; ++k) key = (key ^ pb[k]) * 1099511628211ull;
    };
    mix(&matLight, sizeof(glm::mat4));
    size_t cMoving = 0;
    for (const WSDRAW& d : s_ad)
    {
        if (d.fMoving) { ++cMoving; continue; }
        mix(&d.vao, sizeof(d.vao));
        mix(&d.firstIndex, sizeof(d.firstIndex));
        mix(&d.count, sizeof(d.count));
        mix(d.pmat, sizeof(glm::mat4));
    }
    if (key == 0) key = 1;

    SMSAVE save;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);   // (both sides: the level's winding is not to be relied on)
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);
    s_shWsDepth.Use();
    glUniformMatrix4fv(s_shWsDepth.Loc("matWorldToLight"), 1, GL_FALSE, glm::value_ptr(matLight));
    const GLint locModel = s_shWsDepth.Loc("matModel"), locCBone = s_shWsDepth.Loc("cbone"), locBoneFirst = s_shWsDepth.Loc("iboneFirst");
    GLint ssboPrev = 0;
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 7, &ssboPrev);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssboWs);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(std::max<size_t>(1, s_amatBoneWs.size()) * sizeof(glm::mat4)),
                 s_amatBoneWs.empty() ? nullptr : s_amatBoneWs.data(), GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, s_ssboWs);
    auto drawMap = [&](bool fMoving) {
        GLuint vaoCur = 0;
        for (const WSDRAW& d : s_ad)
        {
            if (d.fMoving != fMoving) continue;
            if (d.vao != vaoCur) glBindVertexArray(vaoCur = d.vao);
            glUniform1i(locCBone, GLint(d.cbone));
            glUniform1i(locBoneFirst, GLint(d.iboneFirst));
            glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(*d.pmat));
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(d.count), GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(d.firstIndex) * 4), d.baseVertex);
        }
    };
    BeginAddedGpuTimer(AGT_WsMap);
    // the still casters' map, when what it was drawn from changed
    if (key != s_keyWsStatic)
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboWsStatic);
        glViewport(0, 0, kcWsMap, kcWsMap);
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        drawMap(false);
        s_keyWsStatic = key;
        ++s_cWsStaticDrawn;
    }
    // the moving ones over a copy of it (none: the still map as it is)
    GLuint texMap = s_texWsStatic;
    if (cMoving > 0)
    {
        glCopyImageSubData(s_texWsStatic, GL_TEXTURE_2D, 0, 0, 0, 0, s_texWs, GL_TEXTURE_2D, 0, 0, 0, 0, kcWsMap, kcWsMap, 1);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboWs);
        glViewport(0, 0, kcWsMap, kcWsMap);
        drawMap(true);
        texMap = s_texWs;
    }
    EndAddedGpuTimer(AGT_WsMap);
    static size_t s_cMovingPrinted = size_t(-1);
    if (std::getenv("SLY2N_WSPRINT") && cMoving != s_cMovingPrinted)
        std::printf("world shadows: %zu moving draws; the still map drawn %d times so far\n", s_cMovingPrinted = cMoving, s_cWsStaticDrawn);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, GLuint(ssboPrev));

    // the frame: its depth copied, every pixel looked up
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShDS);
    glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, dx, dy);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, s_texShDS);
    glBindSampler(3, 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, texMap);
    glBindSampler(4, 0);
    GLint texUnit5 = 0, samplerUnit5 = 0;
    glActiveTexture(GL_TEXTURE5);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texUnit5);
    glGetIntegerv(GL_SAMPLER_BINDING, &samplerUnit5);
    glBindTexture(GL_TEXTURE_2D, s_fCharMaskFrame ? s_texCharMask : 0);
    glBindSampler(5, 0);
    glActiveTexture(GL_TEXTURE0);
    s_shWsApply.Use();
    glUniform1i(s_shWsApply.Loc("texDepth"), 3);
    glUniform1i(s_shWsApply.Loc("texWs"), 4);
    glUniform1i(s_shWsApply.Loc("texMask"), 5);
    glUniform1i(s_shWsApply.Loc("fMask"), s_fCharMaskFrame ? 1 : 0);
    const glm::mat4 matClipToWorld = glm::inverse(matWorldToClip);
    glUniformMatrix4fv(s_shWsApply.Loc("matClipToWorld"), 1, GL_FALSE, glm::value_ptr(matClipToWorld));
    glUniformMatrix4fv(s_shWsApply.Loc("matLight"), 1, GL_FALSE, glm::value_ptr(matLight));
    glUniform3f(s_shWsApply.Loc("dirLight"), L.x, L.y, L.z);
    glUniform2f(s_shWsApply.Loc("viewSize"), float(dx), float(dy));
    glUniform1f(s_shWsApply.Loc("rcpMap"), 1.0f / float(kcWsMap));
    glUniform1f(s_shWsApply.Loc("texelWorld"), texel);
    glUniform1f(s_shWsApply.Loc("strength"), float(NWorldShadowsNow()) / 100.0f);
    // (the lamps of this frame, unit 6: where one lights the surface, the level's shadow fades)
    GLint texCube6 = 0, sampler6 = 0;
    glActiveTexture(GL_TEXTURE6);
    glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP_ARRAY, &texCube6);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler6);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, s_clampFrame > 0 ? s_texLs : 0);
    glBindSampler(6, 0);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(s_shWsApply.Loc("texLs"), 6);
    SetLampUniforms(s_shWsApply);
    glBindVertexArray(s_vaoEmpty);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);   // (the colour darkened; the alpha kept)
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    BeginAddedGpuTimer(AGT_WsApply);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    EndAddedGpuTimer(AGT_WsApply);
    glActiveTexture(GL_TEXTURE5);   // (units 5 and 6 put back: SMSAVE keeps units 3 and 4)
    glBindTexture(GL_TEXTURE_2D, GLuint(texUnit5));
    glBindSampler(5, GLuint(samplerUnit5));
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, GLuint(texCube6));
    glBindSampler(6, GLuint(sampler6));
    glActiveTexture(GL_TEXTURE0);
}

// ------------------------------------------------------------ PC debug: the level shadows' casters shown
// Every group the frame drew (not into render targets, not over the HUD) drawn again over the picture in a colour
// that tells how DrawWorldShadows sorts it, where it is the frame's surface (GEQUAL against the frame's depth, a little
// nearer: as the Shadow Cloak's silhouette); the group under the centre of the picture drawn brighter. The picker: the
// same groups, numbered, into an RGBA8 target against a copy of the frame's depth, only its centre pixel (a scissor),
// read back at once (a stall of one pixel, the debug overlay only).
GLSHADER s_shWsDebug;
int s_stateWsDebug = 0;   // 0 not yet, 1 built, -1 failed
GLuint s_fboPick = 0, s_texPick = 0, s_ssboWsDebug = 0, s_texDSPick = 0;
int s_dxPick = 0, s_dyPick = 0;
WSPICK s_wspick;

bool FEnsureWsDebug(int dx, int dy)
{
    if (s_stateWsDebug == 0)
    {
        s_stateWsDebug = -1;
        if (s_shWsDebug.FInit("smdepth.vert", "wsdebug.frag", "")) s_stateWsDebug = 1, glGenBuffers(1, &s_ssboWsDebug);
        std::printf("shadow casters overlay: %s\n", s_stateWsDebug > 0 ? "built" : "did not build");
    }
    if (s_stateWsDebug < 0 || !FEnsureDsCopy(dx, dy)) return false;
    if (s_fboPick && dx == s_dxPick && dy == s_dyPick && s_texDSPick == s_texShDS) return true;
    if (s_fboPick) glDeleteFramebuffers(1, &s_fboPick), s_fboPick = 0;
    if (s_texPick) glDeleteTextures(1, &s_texPick), s_texPick = 0;
    GLint activePrev = 0, texPrev = 0, fboPrev = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activePrev);
    glActiveTexture(GL_TEXTURE5);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texPrev);
    glGenTextures(1, &s_texPick);
    glBindTexture(GL_TEXTURE_2D, s_texPick);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, dx, dy);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, GLuint(texPrev));
    glActiveTexture(GLenum(activePrev));
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboPrev);
    glGenFramebuffers(1, &s_fboPick);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboPick);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_texPick, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_texShDS, 0);
    const GLenum buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buf);
    const bool fOk = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fboPrev));
    if (!fOk)
    {
        std::printf("shadow casters overlay: the picker's framebuffer is not usable here\n");
        s_stateWsDebug = -1;
        return false;
    }
    s_dxPick = dx;
    s_dyPick = dy;
    s_texDSPick = s_texShDS;
    return true;
}

void DrawShadowCasterOverlay(const SCENE& scene, const glm::mat4& matWorldToClip, const glm::vec3& posCamera, const glm::vec3& dirCamera,
                             GLuint fbo, int dx, int dy)
{
    s_wspick = WSPICK{};
    if (!FEnsureWsDebug(dx, dy)) return;
    const WSBOX box = BoxWorldShadows(posCamera, dirCamera);
    struct DBGDRAW { GLuint vao; int32_t baseVertex; uint32_t firstIndex, count; const glm::mat4* pmat; uint32_t iboneFirst, cbone; int why; uint32_t ipick; };
    static std::vector<DBGDRAW> s_ad;
    static std::vector<glm::mat4> s_amatBoneDbg;
    static std::vector<WSPICK> s_apick;
    s_ad.clear();
    s_amatBoneDbg.clear();
    s_apick.clear();
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.pglobset || sgs.irtgt >= 0 || sgs.fForeground) continue;
        const WSWHY whySet = WsWhySet(sgs, box);
        const GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            if (i >= sgs.afVisible.size() || !sgs.afVisible[i] || i >= sgs.amatGlob.size()) continue;
            const GLOB& glob = gs.aglob[i];
            const GLOB* pgeom = PglobGeometry(gs, i);
            if (!pgeom) continue;   // (nothing to show)
            WSPICK pick;
            pick.fValid = true;
            pick.oid = sgs.oid;
            pick.ib = sgs.ib;
            pick.iglob = int(i);
            pick.cglob = int(gs.aglob.size());
            pick.kind = glob.rp & 0x1F;
            pick.sRadius = sgs.fBounds ? sgs.sBoundsRadius : 0.0f;
            pick.why = whySet != WSWHY_Casts ? whySet : WsWhyGlob(sgs, glob, *pgeom, sgs.amatGlob[i], box);
            const GLGLOB* pgl = i < sgs.apglInst.size() && sgs.apglInst[i] && sgs.apglInst[i]->vao ? sgs.apglInst[i] : pgeom->pgl;
            uint32_t iboneFirst = 0, cbone = 0;
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty())
            {
                iboneFirst = uint32_t(s_amatBoneDbg.size());
                cbone = uint32_t(sgs.aamatBone[i].size());
                s_amatBoneDbg.insert(s_amatBoneDbg.end(), sgs.aamatBone[i].begin(), sgs.aamatBone[i].end());
                pick.fSkinned = true;
            }
            const uint32_t ipick = uint32_t(s_apick.size());
            for (const SUBGLOB& sub : pgeom->asubglob)
            {
                if (!sub.indexCount) continue;
                const WSWHY whySub = WsWhySub(sub);
                if (whySub == WSWHY_Casts) ++pick.csubCast;
                else
                {
                    if (pick.csubLeft++ == 0) pick.whySub = whySub;
                    if (whySub == WSWHY_Empty) continue;   // (no shader: not drawn by the frame either)
                }
                const int why = pick.why != WSWHY_Casts ? pick.why : whySub;
                s_ad.push_back({pgl->vao, pgl->baseVertex, pgl->firstIndex + sub.firstIndex, sub.indexCount, &sgs.amatGlob[i], iboneFirst, cbone, why, ipick});
            }
            if (pick.why == WSWHY_Casts && pick.csubCast == 0) pick.why = pick.whySub;   // (all its parts left out)
            s_apick.push_back(pick);
        }
    }

    SMSAVE save;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GEQUAL);   // (reversed Z: where the group is the frame's surface)
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 64.0f);
    s_shWsDebug.Use();
    glUniformMatrix4fv(s_shWsDebug.Loc("matWorldToLight"), 1, GL_FALSE, glm::value_ptr(matWorldToClip));
    const GLint locModel = s_shWsDebug.Loc("matModel"), locCBone = s_shWsDebug.Loc("cbone"), locBoneFirst = s_shWsDebug.Loc("iboneFirst"),
                locRgba = s_shWsDebug.Loc("rgba");
    GLint ssboPrev = 0;
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 7, &ssboPrev);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssboWsDebug);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(std::max<size_t>(1, s_amatBoneDbg.size()) * sizeof(glm::mat4)),
                 s_amatBoneDbg.empty() ? nullptr : s_amatBoneDbg.data(), GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, s_ssboWsDebug);
    auto draw = [&](const DBGDRAW& d, GLuint& vaoCur) {
        if (d.vao != vaoCur) glBindVertexArray(vaoCur = d.vao);
        glUniform1i(locCBone, GLint(d.cbone));
        glUniform1i(locBoneFirst, GLint(d.iboneFirst));
        glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(*d.pmat));
        glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(d.count), GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(d.firstIndex) * 4), d.baseVertex);
    };

    // the picker: the centre pixel's group (its number + 1 in the colour; 0 none)
    int ipickHit = -1;
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShDS);
        glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboPick);
        glViewport(0, 0, dx, dy);
        const int xC = dx / 2, yC = dy / 2;
        glEnable(GL_SCISSOR_TEST);
        glScissor(xC, yC, 1, 1);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        GLuint vaoCur = 0;
        for (const DBGDRAW& d : s_ad)
        {
            const uint32_t n = d.ipick + 1;
            glUniform4f(locRgba, float(n & 0xFF) / 255.0f, float((n >> 8) & 0xFF) / 255.0f, float((n >> 16) & 0xFF) / 255.0f, 1.0f);
            draw(d, vaoCur);
        }
        uint8_t ab[4] = {};
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fboPick);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(xC, yC, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, ab);
        const uint32_t n = uint32_t(ab[0]) | (uint32_t(ab[1]) << 8) | (uint32_t(ab[2]) << 16);
        if (n > 0 && n <= s_apick.size()) ipickHit = int(n - 1), s_wspick = s_apick[n - 1];
        glDisable(GL_SCISSOR_TEST);
    }

    // the colours over the frame
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, dx, dy);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);   // (the alpha kept)
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    static uint32_t s_cframePulse = 0;
    const float tPulse = 0.5f + 0.5f * std::sin(float(++s_cframePulse) * 0.2f);
    GLuint vaoCur = 0;
    for (const DBGDRAW& d : s_ad)
    {
        glm::vec4 rgba;
        switch (d.why)
        {
        case WSWHY_Casts: rgba = glm::vec4(0.15f, 0.95f, 0.25f, 0.35f); break;
        case WSWHY_Character: rgba = glm::vec4(0.25f, 0.45f, 1.0f, 0.35f); break;
        case WSWHY_LightVolume: case WSWHY_Ps2Shadow: case WSWHY_Glow: rgba = glm::vec4(1.0f, 0.6f, 0.1f, 0.40f); break;
        default: rgba = glm::vec4(1.0f, 0.15f, 0.15f, 0.40f); break;
        }
        if (int(d.ipick) == ipickHit) rgba = glm::mix(rgba, glm::vec4(1.0f, 1.0f, 0.3f, 0.75f), 0.35f + 0.4f * tPulse);
        glUniform4fv(locRgba, 1, glm::value_ptr(rgba));
        draw(d, vaoCur);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, GLuint(ssboPrev));
}

// ------------------------------------------------------------ PC debug: the texture picker (display.h texturePicker)
// Every part of every group the frame drew, numbered into the picker's target (as the casters' overlay's), the centre
// pixel read back; the part drawn brighter over the picture; its texture's mod name found once per texture.
TEXPICK s_texpick;
const BMP* s_pbmpPick = nullptr;
const CLUT* s_pclutPick = nullptr;
MODELEXPORT s_mxPick;   // (the picked object's model and its pose this frame: "Export the picked model")

void DrawTexturePicker(const SCENE& scene, const glm::mat4& matWorldToClip, GLuint fbo, int dx, int dy)
{
    s_texpick = TEXPICK{};
    s_mxPick = MODELEXPORT{};
    if (!FEnsureWsDebug(dx, dy)) return;
    struct TPDRAW { GLuint vao; int32_t baseVertex; uint32_t firstIndex, count; const glm::mat4* pmat; uint32_t iboneFirst, cbone;
                    const SCENEGLOBSET* psgs; const SUBGLOB* psub; int iglob, isub, csub; };
    static std::vector<TPDRAW> s_ad;
    static std::vector<glm::mat4> s_amatBone;
    s_ad.clear();
    s_amatBone.clear();
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.pglobset || sgs.irtgt >= 0 || sgs.fForeground) continue;
        const GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            if (i >= sgs.afVisible.size() || !sgs.afVisible[i] || i >= sgs.amatGlob.size()) continue;
            const GLOB* pgeom = PglobGeometry(gs, i);
            if (!pgeom) continue;
            const GLGLOB* pgl = i < sgs.apglInst.size() && sgs.apglInst[i] && sgs.apglInst[i]->vao ? sgs.apglInst[i] : pgeom->pgl;
            uint32_t iboneFirst = 0, cbone = 0;
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty())
            {
                iboneFirst = uint32_t(s_amatBone.size());
                cbone = uint32_t(sgs.aamatBone[i].size());
                s_amatBone.insert(s_amatBone.end(), sgs.aamatBone[i].begin(), sgs.aamatBone[i].end());
            }
            const int csub = int(pgeom->asubglob.size());
            for (int k = 0; k < csub; ++k)
            {
                const SUBGLOB& sub = pgeom->asubglob[size_t(k)];
                if (!sub.pshd || !sub.indexCount || sub.pshd->shdk == 2 || sub.pshd->shdk == 3) continue;
                s_ad.push_back({pgl->vao, pgl->baseVertex, pgl->firstIndex + sub.firstIndex, sub.indexCount, &sgs.amatGlob[i], iboneFirst, cbone,
                                &sgs, &sub, int(i), k, csub});
            }
        }
    }
    if (s_ad.empty()) return;

    SMSAVE save;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GEQUAL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 64.0f);
    s_shWsDebug.Use();
    glUniformMatrix4fv(s_shWsDebug.Loc("matWorldToLight"), 1, GL_FALSE, glm::value_ptr(matWorldToClip));
    const GLint locModel = s_shWsDebug.Loc("matModel"), locCBone = s_shWsDebug.Loc("cbone"), locBoneFirst = s_shWsDebug.Loc("iboneFirst"),
                locRgba = s_shWsDebug.Loc("rgba");
    glUniform1f(s_shWsDebug.Loc("sTowardEye"), 0.0f);
    GLint ssboPrev = 0;
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 7, &ssboPrev);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssboWsDebug);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(std::max<size_t>(1, s_amatBone.size()) * sizeof(glm::mat4)),
                 s_amatBone.empty() ? nullptr : s_amatBone.data(), GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, s_ssboWsDebug);
    auto draw = [&](const TPDRAW& d, GLuint& vaoCur) {
        if (d.vao != vaoCur) glBindVertexArray(vaoCur = d.vao);
        glUniform1i(locCBone, GLint(d.cbone));
        glUniform1i(locBoneFirst, GLint(d.iboneFirst));
        glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(*d.pmat));
        glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(d.count), GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(d.firstIndex) * 4), d.baseVertex);
    };
    // the picker: the centre pixel's part (its number + 1 in the colour)
    int iHit = -1;
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShDS);
        glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboPick);
        glViewport(0, 0, dx, dy);
        const int xC = dx / 2, yC = dy / 2;
        glEnable(GL_SCISSOR_TEST);
        glScissor(xC, yC, 1, 1);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        GLuint vaoCur = 0;
        for (size_t k = 0; k < s_ad.size(); ++k)
        {
            const uint32_t n = uint32_t(k) + 1;
            glUniform4f(locRgba, float(n & 0xFF) / 255.0f, float((n >> 8) & 0xFF) / 255.0f, float((n >> 16) & 0xFF) / 255.0f, 1.0f);
            draw(s_ad[k], vaoCur);
        }
        uint8_t ab[4] = {};
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fboPick);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(xC, yC, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, ab);
        const uint32_t n = uint32_t(ab[0]) | (uint32_t(ab[1]) << 8) | (uint32_t(ab[2]) << 16);
        if (n > 0 && n <= s_ad.size()) iHit = int(n - 1);
        glDisable(GL_SCISSOR_TEST);
    }
    if (iHit >= 0)
    {
        const TPDRAW& d = s_ad[size_t(iHit)];
        TEXPICK& p = s_texpick;
        p.fValid = true;
        p.oid = d.psgs->oid;
        p.iglob = d.iglob;
        p.cglob = int(d.psgs->pglobset->aglob.size());
        p.isub = d.isub;
        p.csub = d.csub;
        s_mxPick.pglobset = d.psgs->pglobset;
        s_mxPick.amatGlob = d.psgs->amatGlob;
        s_mxPick.aamatBone = d.psgs->aamatBone;
        s_mxPick.afVisible = d.psgs->afVisible;
        s_mxPick.oid = d.psgs->oid;
        // its first textured pass's texture (the one the frame shows: its animation's frame)
        const SHD* pshd = d.psub->pshd;
        for (const SHDPASS& ps : pshd->apass)
        {
            if (!ps.fTextured) continue;
            const TEXSEL sel = TexselFromShd(pshd, d.psub->psaa ? d.psub->psaa->iframe : 0, ps.itexPass);
            if (!sel.pbmp) break;
            p.fTextured = true;
            p.dx = sel.pbmp->dx;
            p.dy = sel.pbmp->dy;
            static std::pair<const BMP*, const CLUT*> s_keyName{nullptr, nullptr};
            static std::string s_name;
            if (s_keyName.first != sel.pbmp || s_keyName.second != sel.pclut)
            {
                std::vector<uint8_t> rgba;
                std::string err;
                s_name = FDecodeTexture(sel.pbmp, sel.pclut, rgba, err) ? SzTextureModName(rgba, p.dx, p.dy) : std::string();
                s_keyName = {sel.pbmp, sel.pclut};
            }
            p.name = s_name;
            p.nSource = p.name.empty() ? 0 : NTextureModSource(p.name);
            s_pbmpPick = sel.pbmp;
            s_pclutPick = sel.pclut;
            break;
        }
        // the part drawn brighter over the picture (a pulse)
        static uint32_t s_cframePulse = 0;
        const float u = 0.5f + 0.5f * std::sin(float(++s_cframePulse) * 0.2f);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, dx, dy);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glUniform4f(locRgba, 1.0f, 0.95f, 0.3f, 0.2f + 0.3f * u);
        GLuint vaoCur = 0;
        draw(d, vaoCur);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, GLuint(ssboPrev));
}
#endif
}  // namespace

void UpdateGlobColorsGpu(const GLOB& glob) { UpdateGlobColorsGpu(glob, glob.pgl); }

void UpdateGlobVerticesGpu(const GLOB& glob)
{
    const GLGLOB* pgl = glob.pgl;
    if (!pgl || !pgl->vbo || glob.meshk != MESHK_Strips || glob.avtx.empty()) return;
    static std::vector<GPUVTX> s_av;
    GpuVerticesOfGlob(glob, s_av);
    glBindBuffer(GL_ARRAY_BUFFER, pgl->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(size_t(pgl->baseVertex) * sizeof(GPUVTX)), GLsizeiptr(s_av.size() * sizeof(GPUVTX)), s_av.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    UpdateGlobColorsGpu(glob, pgl);
}

void SetGlobBoundsGpu(const GLOB& glob, const glm::vec3& posCenter, float sRadius)
{
    if (!glob.pgl) return;
    glob.pgl->posCenter = posCenter;
    glob.pgl->sRadius = sRadius;
}

void UpdateGlobColorsGpu(const GLOB& glob, const GLGLOB* pgl)
{
    if (!pgl || !pgl->vboColor || glob.meshk != MESHK_Strips || glob.avtx.empty()) return;
    static std::vector<GPUCOL> s_acol;
    s_acol.resize(glob.avtx.size());
    uint8_t aMin = 255;
    for (size_t i = 0; i < s_acol.size(); ++i)
    {
        std::memcpy(s_acol[i].rgbaA, &glob.avtx[i].rgbaA, 4);
        std::memcpy(s_acol[i].rgbaB, &glob.avtx[i].rgbaB, 4);
        aMin = std::min(aMin, s_acol[i].rgbaA[3]);
    }
    pgl->aMinA = aMin;
    glBindBuffer(GL_ARRAY_BUFFER, pgl->vboColor);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(size_t(pgl->baseVertex) * sizeof(GPUCOL)), GLsizeiptr(s_acol.size() * sizeof(GPUCOL)), s_acol.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

#ifdef __SWITCH__
void PackGlobColors(const GLOB& glob, GLOBCOLORS& cols)
{
    cols.ab.resize(glob.avtx.size() * sizeof(GPUCOL));
    GPUCOL* acol = reinterpret_cast<GPUCOL*>(cols.ab.data());
    uint8_t aMin = 255;
    for (size_t i = 0; i < glob.avtx.size(); ++i)
    {
        std::memcpy(acol[i].rgbaA, &glob.avtx[i].rgbaA, 4);
        std::memcpy(acol[i].rgbaB, &glob.avtx[i].rgbaB, 4);
        aMin = std::min(aMin, acol[i].rgbaA[3]);
    }
    cols.aMin = aMin;
}

void UploadGlobColorsGpu(const GLOB& glob, const GLGLOB* pgl, const GLOBCOLORS& cols)
{
    if (!pgl || !pgl->vboColor || glob.meshk != MESHK_Strips || glob.avtx.empty()) return;
    pgl->aMinA = cols.aMin;
    glBindBuffer(GL_ARRAY_BUFFER, pgl->vboColor);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(size_t(pgl->baseVertex) * sizeof(GPUCOL)), GLsizeiptr(cols.ab.size()), cols.ab.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
#endif

GLGLOB* PglInstanceCopyGpu(const GLOB& glob)
{
    if (glob.meshk != MESHK_Strips || glob.avtx.empty() || glob.aindex.empty()) return nullptr;
    auto pgl = std::make_unique<GLGLOB>();
    std::vector<GPUVTX> av;
    GpuVerticesOfGlob(glob, av);
    AppendArena(pgl.get(), av, glob.aindex, &glob.anrmSmooth);
    GLGLOB* p = pgl.get();
    s_aglglob.push_back(std::move(pgl));
    return p;
}

void FlushInstanceGlobsGpu() { FlushArena(); }

void ResetTextureCache()
{
    // The cache is keyed by the old world's BMP / CLUT records, which ResetShaders frees: a new world's record may get
    // a freed one's address (the wrong texture), and the old textures would stay in GPU memory for the whole session
    // (with texture mods, ~400 MiB per world at 4x: MODDING.md "Limits").
    for (const auto& [key, tex] : s_mptex)
        if (tex && tex != s_texWhite)
        {
            s_atexFreed.push_back(tex);
            ForgetModTexture(tex);
        }
    s_mptex.clear();
    s_settexMip.clear();
    s_aallpass.clear();
    ++s_genTexCache;
}

unsigned GlTextureOfShd(const SHD* pshd, int iframe)
{
    InitCommon();
    return TexFromSel(TexselFromShd(pshd, iframe, 0));
}

glm::vec3 CAMERA::Dir() const
{
    if (fMat) return mat[0];
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    return glm::vec3(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p));
}

glm::mat4 CAMERA::MatWorldToClip(float aspect) const
{
    // Reversed-Z infinite-far perspective (clip control 0..1): depth = near / w, greater is nearer.
    const float f = 1.0f / std::tan(glm::radians(fovY) * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = f / aspect;
    proj[1][1] = f;
    proj[2][3] = -1.0f;
    proj[3][2] = sNear;
    if (fMat)
    {
        // GL eye axes from the camera's rows: right = -y, up = z, back = -x
        glm::mat4 eye(1.0f);
        eye[0] = glm::vec4(-mat[1], 0.0f);
        eye[1] = glm::vec4(mat[2], 0.0f);
        eye[2] = glm::vec4(-mat[0], 0.0f);
        eye[3] = glm::vec4(pos, 1.0f);
        return proj * glm::inverse(eye);
    }
    const glm::mat4 view = glm::lookAt(pos, pos + Dir(), glm::vec3(0, 0, 1));
    return proj * view;
}

void BuildSceneGpu(SCENE& scene)
{
    InitCommon();
    for (SCENEGLOBSET& sgs : scene.aglobset)
        for (GLOB& glob : sgs.pglobset->aglob)
            BuildGlobGpu(glob);
    FlushArena();
    for (int iprog = 0; iprog < 2; ++iprog)
    {
        const GLSHADER& sh = *s_apshGlob[iprog];
        UNIFORMS& u = s_au[iprog];
        u.matWorldToClip = sh.Loc("matWorldToClip");
        u.icolor = sh.Loc("icolor");
        u.rgbaConst = sh.Loc("rgbaConst");
        u.posCamera = sh.Loc("posCamera");
        u.dirCamera = sh.Loc("dirCamera");
        u.tex = sh.Loc("tex");
        u.fTextured = sh.Loc("fTextured");
        u.ate = sh.Loc("ate");
        u.atst = sh.Loc("atst");
        u.aref = sh.Loc("aref");
        u.afailMode = sh.Loc("afailMode");
        u.fogColor = sh.Loc("fogColor");
        u.fFogBlack = sh.Loc("fFogBlack");
        u.fDate = sh.Loc("fDate");
        u.texDate = sh.Loc("texDate");
    }
    s_pu = &s_au[0];
    s_uc.matWorldToClip = glCelBorderShader.Loc("matWorldToClip");
    s_uc.matModel = glCelBorderShader.Loc("matModel");
    s_uc.halfWidthNdc = glCelBorderShader.Loc("halfWidthNdc");
    s_uc.edgeBase = glCelBorderShader.Loc("edgeBase");
    s_uc.celRgba = glCelBorderShader.Loc("celRgba");
    s_uc.cbone = glCelBorderShader.Loc("cbone");
    s_uc.iboneFirst = glCelBorderShader.Loc("iboneFirst");
    s_uc.csegSmooth = glCelBorderShader.Loc("csegSmooth");
    if (!s_ssboRec)
    {
        glGenBuffers(1, &s_ssboBone);
        glGenBuffers(1, &s_ssboRec);
        glGenBuffers(1, &s_ssboDraw);
        glGenBuffers(1, &s_bufIndirect);
        glGenBuffers(1, &s_ssboWarp);
        glGenBuffers(1, &s_ssboWarpMat);
        glGenBuffers(1, &s_ssboTexh);
    }
}

namespace
{
// One view of the scene into one target: the frame (irtgt -1: the globsets not drawn into a render target) or a
// render target (irtgt 0 / 1: only its globsets, their kinds remapped as RenderGlobCtx does through 0x1BEE10).
struct PASSVIEW
{
    glm::mat4 matWorldToClip{1.0f};
    glm::vec3 pos{0.0f}, dir{1, 0, 0};
    GLuint fbo = 0;
    int dx = 1, dy = 1;
    float aspect = 1.0f;        // the target's pixel aspect (cel-border widths)
    float rMRDAdjust = 1.0f;
    uint32_t grfChannels = 0xFF;   // CM +0x14 (the volume tree's channel test)
    int irtgt = -1;
    bool fForeground = false;   // the foreground kinds 22..25 only (after the HUD, over a cleared depth)
    uint32_t rgbaClear = 0;
    uint8_t alphaClear = 0;
};

// orig: 0x002C0B28 / 0x002C0B90 (D_002C0BF8's per-target tables: a group kind -> its kind in the target, -1 not
// drawn): 3 / 4 -> 15 (18), 5 / 6 -> 16 (19), 9 -> 17 (20), the fog kinds and the target kinds likewise.
const int8_t s_amapkindRtgt[2][26] = {
    {-1, -1, -1, 15, 15, 16, 16, -1, -1, 17, -1, -1, -1, -1, -1, 15, 16, 17, 15, 16, 17, -1, 15, 16, 17, -1},
    {-1, -1, -1, 18, 18, 19, 19, -1, -1, 20, -1, -1, -1, -1, -1, 18, 19, 20, 18, 19, 20, -1, 18, 19, 20, -1},
};

// The regroup keys (order << 52 | state << 28 | index) ascending. The keys are distinct (the index), so any
// sort gives the same order: a stable LSD radix sort on bits 28..63 (the index part is already ascending)
// for the long segments, std::sort for the short ones.
void SortKeysUnique(std::vector<uint64_t>& akey)
{
    if (akey.size() < 512)
    {
        std::sort(akey.begin(), akey.end());
        return;
    }
    static std::vector<uint64_t> s_atmp;
    static uint32_t s_ac[4096];
    s_atmp.resize(akey.size());
    std::vector<uint64_t>* psrc = &akey;
    std::vector<uint64_t>* pdst = &s_atmp;
    for (int shift = 28; shift < 64; shift += 12)
    {
        std::fill(std::begin(s_ac), std::end(s_ac), 0u);
        for (uint64_t k : *psrc) ++s_ac[(k >> shift) & 0xFFF];
#ifdef __SWITCH__
        // (the Switch: a digit every key shares orders nothing: that pass is skipped)
        if (s_ac[(psrc->front() >> shift) & 0xFFF] == psrc->size()) continue;
#endif
        uint32_t sum = 0;
        for (uint32_t& c : s_ac) { const uint32_t n = c; c = sum; sum += n; }
        for (uint64_t k : *psrc) (*pdst)[s_ac[(k >> shift) & 0xFFF]++] = k;
        std::swap(psrc, pdst);
    }
    if (psrc != &akey) akey.swap(*psrc);   // (an odd number of passes: the result is in the other buffer)
}

// orig: VU1 P_G 14-82 (the billboard modes: LoadGlob 0x167320 passes the group's S8 rtck to the type-0 loader, which
// makes rtck 0 / 1 / 2 the param flags 0x100 / 0x200 / 0x400; P_G then builds the model's basis from the camera
// position): Sly 1's RTCK with the values one lower (-1 none). 0: the model's x axis toward the camera, z the
// camera's up made square to it (Sly 1 RTCK_All, AdjustAloRtckMat); 1: x toward the camera about the world's z
// (RTCK_WorldZ). The axes keep the matrix's scales; the model turns about its origin. 2 (only on effect meshes in
// Paris): not changed.
// TODO: check the pivot and mode 2 against P_G (not disassembled here).
static bool FBillboardMatrix(const glm::mat4& m, int rtck, const glm::vec3& posCam, const glm::vec3& vecUpCam, glm::mat4& mOut)
{
    if (rtck != 0 && rtck != 1) return false;
    static const bool s_fOff = std::getenv("SLY2_NOBILLBOARD") != nullptr;   // debugging: the loaded rotation
    if (s_fOff) return false;
    const glm::vec3 pos(m[3]);
    glm::vec3 x = posCam - pos;
    if (rtck == 1) x.z = 0.0f;
    if (glm::dot(x, x) < 1e-6f) return false;
    x = glm::normalize(x);
    glm::vec3 z = rtck == 1 ? glm::vec3(0.0f, 0.0f, 1.0f) : vecUpCam - x * glm::dot(vecUpCam, x);
    if (glm::dot(z, z) < 1e-6f) return false;
    z = glm::normalize(z);
    const glm::vec3 y = glm::cross(z, x);
    mOut = glm::mat4(glm::vec4(x * glm::length(glm::vec3(m[0])), 0.0f), glm::vec4(y * glm::length(glm::vec3(m[1])), 0.0f),
                     glm::vec4(z * glm::length(glm::vec3(m[2])), 0.0f), m[3]);
    return true;
}

void RenderPass(const SCENE& scene, const PASSVIEW& pv, const RENDEROPTS& opts)
{
    const float aspect = pv.aspect;
    const glm::mat4& matWorldToClip = pv.matWorldToClip;
    struct { glm::vec3 pos, dir; glm::vec3 Dir() const { return dir; } } cam{pv.pos, pv.dir};
    const float sNearClipSh = g_cm.sNearClip;   // (CM +0x4C: the hulls' near test, DrawShadow)
    // the drawn near plane's corners (NDC depth 1 in the reversed-Z projection): the hulls it reaches count z-fail
    std::array<glm::vec3, 4> aposNear;
    {
        const glm::mat4 m = glm::inverse(matWorldToClip);
        const float ax[4] = {-1.0f, 1.0f, 1.0f, -1.0f}, ay[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        for (size_t i = 0; i < 4; ++i)
        {
            const glm::vec4 v = m * glm::vec4(ax[i], ay[i], 1.0f, 1.0f);
            aposNear[i] = glm::vec3(v) / v.w;
        }
    }
    // The view frustum's planes (Gribb / Hartmann on the clip matrix; reversed Z, infinite far: left, right, bottom,
    // top), normalized so a sphere is outside when its centre is at least its radius behind one.
    glm::vec4 aplane[4];
    {
        const glm::mat4 m = glm::transpose(matWorldToClip);   // rows of the clip matrix
        aplane[0] = m[3] + m[0];
        aplane[1] = m[3] - m[0];
        aplane[2] = m[3] + m[1];
        aplane[3] = m[3] - m[1];
        for (glm::vec4& p : aplane) p /= glm::length(glm::vec3(p));
    }
    CM cmFade;
    cmFade.rMRDAdjust = pv.rMRDAdjust * g_display.drawDistance;
    // (QoL draw distance: the cel borders (default MRD 2000) follow it only below 1x; above, the original's limit
    // stays, or the far characters' fixed-width outlines cover them)
    CM cmFadeCel;
    cmFadeCel.rMRDAdjust = pv.rMRDAdjust * std::min(g_display.drawDistance, 1.0f);
    static const int s_oidFade = std::getenv("SLY2_FADEOID") ? std::atoi(std::getenv("SLY2_FADEOID")) : -2;
    auto maxScale = [](const glm::mat4& m) {
        return std::sqrt(std::max({glm::dot(glm::vec3(m[0]), glm::vec3(m[0])),
                                   glm::dot(glm::vec3(m[1]), glm::vec3(m[1])),
                                   glm::dot(glm::vec3(m[2]), glm::vec3(m[2]))}));
    };
    auto outside = [&](const glm::vec3& pos, float radius) {
        for (int i = 0; i < 4; ++i)
            if (glm::dot(glm::vec3(aplane[i]), pos) + aplane[i].w <= -radius) return true;
        return false;
    };
    static const bool s_fNoFrustum = std::getenv("SLY2_NOFRUSTUM") != nullptr;
    static const bool s_fNoFade = std::getenv("SLY2_NOFADE") != nullptr;   // debugging: no distance fade
    static const bool s_fNoTree = std::getenv("SLY2_NOTREE") != nullptr;   // debugging: the volume tree's culling off

    // Draw records (RenderSw 0x1CDA80 / RenderGlob 0x168638): kind = the group's rp, z = distance squared.
    // (one buffer per target kind: the frame's pass and the render targets' passes keep their records apart,
    // reused across frames without reallocating)
    static std::vector<DRAWREC> s_aarec[1 + CRTGT];
    std::vector<DRAWREC>& arec = s_aarec[size_t(pv.irtgt + 1)];
    arec.clear();
    // the billboarded groups' matrices this pass (a deque: the records point into it)
    static std::deque<glm::mat4> s_aamatBillboard[1 + CRTGT];
    std::deque<glm::mat4>& amatBillboard = s_aamatBillboard[size_t(pv.irtgt + 1)];
    amatBillboard.clear();
    const glm::vec3 vecUpCam = [&] {
        const glm::vec3 r1(matWorldToClip[0][1], matWorldToClip[1][1], matWorldToClip[2][1]);   // the clip matrix's y row
        return glm::dot(r1, r1) > 0.0f ? glm::normalize(r1) : glm::vec3(0.0f, 0.0f, 1.0f);
    }();
    // (into the given deque: the record blocks keep their own, built on several threads)
    auto pmatBillboardIn = [&](std::deque<glm::mat4>& amat, const glm::mat4& m, int rtck) -> const glm::mat4* {
        glm::mat4 mb;
        if (!FBillboardMatrix(m, rtck, cam.pos, vecUpCam, mb)) return &m;
        amat.push_back(mb);
        return &amat.back();
    };
    auto pmatBillboard = [&](const glm::mat4& m, int rtck) { return pmatBillboardIn(amatBillboard, m, rtck); };
    static const char* s_szVolDbg = std::getenv("SLY2_VOLDBG");   // debugging: the volumes (shader kind 3) within this distance
    if (s_szVolDbg && pv.irtgt < 0 && !pv.fForeground)
    {
        static int s_cframe = 0;
        if (++s_cframe == 1)
            for (const SCENEGLOBSET& sgs : scene.aglobset)
                for (size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    const GLOB& gg = !g.pgl && g.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(g.iglobTemplate)] : g;
                    int shdk = -1;
                    for (const SUBGLOB& sub : gg.asubglob) if (sub.pshd) shdk = std::max(shdk, int(sub.pshd->shdk));
                    static const bool s_fFx = std::getenv("SLY2_VOLDBGFX") != nullptr;   // (the type-3 effect meshes instead)
                    if (s_fFx)
                    {
                        if (gg.meshk != MESHK_Indexed || gg.asubfx.empty() || !gg.asubfx[0].pshd) continue;
                        const SHD* ps = gg.asubfx[0].pshd;
                        const glm::vec3 c = i < sgs.amatGlob.size() ? glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f)) : glm::vec3(0);
                        std::printf("fx3: oid %d #%zu rp %d shd %d k%d grfshd 0x%X rgba %08X %08X r %.0f at %.0f %.0f %.0f vis %d\n", sgs.oid, i, g.rp, ps->id, ps->shdk, ps->grfshd,
                                    ps->argba[0], ps->argba[1], g.sRadius, c.x, c.y, c.z, i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1);
                        continue;
                    }
                    if (shdk != 3 && (g.rp & 0x1F) != 11 && (g.rp & 0x1F) != 13) continue;
                    const glm::vec3 c = i < sgs.amatGlob.size() ? glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f)) : glm::vec3(0);
                    const float d = glm::distance(c, cam.pos);
                    if (d > std::atof(s_szVolDbg)) continue;
                    std::printf("vol: oid %d glob@%zu #%zu rp %d shdk %d grfshd 0x%X r %.0f at %.0f %.0f %.0f d %.0f xf %d vis %d hidden %d irtgt %d A %08X\n", sgs.oid, sgs.ib, i,
                                g.rp, shdk, gg.asubglob.empty() || !gg.asubglob[0].pshd ? 0 : gg.asubglob[0].pshd->grfshd, g.sRadius, c.x, c.y, c.z, d,
                                int(sgs.fTransform), i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1, int(g.fHidden), sgs.irtgt,
                                gg.avtx.empty() ? 0 : gg.avtx[0].rgbaA);
                    for (const SUBGLOB& sub : gg.asubglob)
                        if (sub.pshd && sub.pshd->shdk == 3)
                            std::printf("    vshd %d grfshd 0x%X argba %08X %08X %08X tex %zu\n", sub.pshd->id, sub.pshd->grfshd, sub.pshd->argba[0], sub.pshd->argba[1], sub.pshd->argba[2], sub.pshd->atex.size());
                }
    }
    if (static const bool s_fRtckDbg = std::getenv("SLY2_RTCKDBG") != nullptr; s_fRtckDbg && pv.irtgt < 0 && !pv.fForeground)   // debugging: the groups with a billboard mode
    {
        static bool s_f = false;
        if (!s_f)
        {
            s_f = true;
            for (const SCENEGLOBSET& sgs : scene.aglobset)
                for (size_t i = 0; i < sgs.pglobset->aglob.size() && i < sgs.amatGlob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    if (g.rtck == -1) continue;
                    const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f));
                    std::printf("rtck %d: oid %d #%zu rp %d meshk %d r %.0f at %.0f %.0f %.0f vis %d inst %d\n", g.rtck, sgs.oid, i, g.rp, int(g.meshk), g.sRadius, c.x, c.y, c.z,
                                i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1, g.iglobTemplate);
                }
        }
    }
    static const char* s_szPtDbg = std::getenv("SLY2_PTDBG");   // debugging: "x y z": the groups whose sphere holds the point
    if (s_szPtDbg && pv.irtgt < 0 && !pv.fForeground)
    {
        static int s_cframe = 0;
        glm::vec3 pt(0.0f);
        std::sscanf(s_szPtDbg, "%f %f %f", &pt.x, &pt.y, &pt.z);
        if (++s_cframe == 1)
            for (const SCENEGLOBSET& sgs : scene.aglobset)
                for (size_t i = 0; i < sgs.pglobset->aglob.size() && i < sgs.amatGlob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    const GLOB& gg = !g.pgl && g.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(g.iglobTemplate)] : g;
                    const float r = std::max(g.sRadius, gg.pgl ? gg.pgl->sRadius : 0.0f);
                    const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(gg.pgl && !g.fDraw ? gg.pgl->posCenter : g.posCenter, 1.0f));
                    static const float s_rPt = std::getenv("SLY2_PTDBGR") ? float(std::atof(std::getenv("SLY2_PTDBGR"))) : -1.0f;
                    if (glm::distance(c, pt) > (s_rPt > 0.0f ? s_rPt : r)) continue;
                    std::printf("pt: oid %d #%zu n0 %d rp %d meshk %d r %.0f at %.0f %.0f %.0f vis %d xf %d:", sgs.oid, i, g.n0, g.rp, int(gg.meshk), r, c.x, c.y, c.z,
                                i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1, int(sgs.fTransform));
                    for (const SUBGLOB& sub : gg.asubglob) if (sub.pshd) std::printf(" <shd %d k%d grf 0x%X %08X>", sub.pshd->id, sub.pshd->shdk, sub.pshd->grfshd, sub.pshd->argba[0]);
                    for (const auto& sub : gg.asubfx) if (sub.pshd) std::printf(" <fx shd %d k%d grf 0x%X %08X>", sub.pshd->id, sub.pshd->shdk, sub.pshd->grfshd, sub.pshd->argba[0]);
                    std::printf("\n");
                    // (SLY2_PTDBGV=N: also the group's N vertices nearest the point, their colours A / B)
                    if (const char* szV = std::getenv("SLY2_PTDBGV"))
                    {
                        std::vector<std::pair<float, size_t>> ad;
                        for (size_t k = 0; k < gg.avtx.size(); ++k)
                            ad.push_back({glm::distance(glm::vec3(sgs.amatGlob[i] * glm::vec4(gg.avtx[k].pos, 1.0f)), pt), k});
                        std::sort(ad.begin(), ad.end());
                        for (size_t k = 0; k < ad.size() && k < size_t(std::max(1, std::atoi(szV))); ++k)
                        {
                            const VTXGLOB& v = gg.avtx[ad[k].second];
                            const glm::vec3 w = glm::vec3(sgs.amatGlob[i] * glm::vec4(v.pos, 1.0f));
                            std::printf("    v%zu at %.0f %.0f %.0f d %.0f A %08X B %08X unbaked %d\n", ad[k].second, w.x, w.y, w.z, ad[k].first,
                                        v.rgbaA, v.rgbaB, int(v.fUnbaked));
                        }
                    }
                }
    }
    // The records built in items on the pass helpers: runs of whole entries, or a large entry's groups in parts (its
    // own tests first, on this thread). An item writes only its own block; joined in order: the records of one loop.
    struct SGSPRE { bool fDraw = false, fTree = false; float wholeFade = 1.0f; };
    // (a cache line or more each: two threads filling neighbouring items do not share one)
    struct alignas(64) RECBLOCK { std::vector<DRAWREC> arec; std::deque<glm::mat4> amatBillboard; int cmrd = 0, cculled = 0; };
    // whole entries [isgsFirst, isgsLim) (isplit < 0), or the groups [iglobFirst, iglobLim) of entry isgsFirst (the
    // last part to the end: iglobLim SIZE_MAX), whose tests are apre[isplit]
    struct RECITEM { uint32_t isgsFirst = 0, isgsLim = 0; size_t iglobFirst = 0, iglobLim = 0; int isplit = -1; };
    constexpr size_t CGLOB_ITEM = 128, CSGS_ITEM = 32;
    struct RECITEMS { std::vector<RECITEM> aitem; size_t csgs = 0; const SCENEGLOBSET* psgs = nullptr; int csplit = 0; };
    static RECITEMS s_aitems[1 + CRTGT];
    RECITEMS& items = s_aitems[size_t(pv.irtgt + 1)];
    if (items.csgs != scene.aglobset.size() || items.psgs != scene.aglobset.data())
    {
        items = RECITEMS{};
        items.csgs = scene.aglobset.size();
        items.psgs = scene.aglobset.data();
        RECITEM run;
        size_t cglobRun = 0;
        auto endRun = [&](uint32_t isgs) {
            if (run.isgsLim > run.isgsFirst) items.aitem.push_back(run);
            run = RECITEM{};
            run.isgsFirst = run.isgsLim = isgs;
            cglobRun = 0;
        };
        for (uint32_t isgs = 0; isgs < uint32_t(scene.aglobset.size()); ++isgs)
        {
            const size_t cglob = scene.aglobset[isgs].pglobset->aglob.size();
            if (cglob > CGLOB_ITEM)
            {
                endRun(isgs);
                const int isplit = items.csplit++;
                for (size_t i = 0; i < cglob; i += CGLOB_ITEM)
                {
                    RECITEM part;
                    part.isgsFirst = isgs;
                    part.isgsLim = isgs + 1;
                    part.iglobFirst = i;
                    part.iglobLim = i + CGLOB_ITEM >= cglob ? SIZE_MAX : i + CGLOB_ITEM;
                    part.isplit = isplit;
                    items.aitem.push_back(part);
                }
                endRun(isgs + 1);
                continue;
            }
            run.isgsLim = isgs + 1;
            cglobRun += cglob;
            if (cglobRun >= CGLOB_ITEM || run.isgsLim - run.isgsFirst >= CSGS_ITEM) endRun(isgs + 1);
        }
        endRun(uint32_t(scene.aglobset.size()));
    }
    static std::vector<RECBLOCK> s_aablock[1 + CRTGT];
    std::vector<RECBLOCK>& ablock = s_aablock[size_t(pv.irtgt + 1)];
    if (ablock.size() < items.aitem.size()) ablock.resize(items.aitem.size());
    static std::vector<SGSPRE> s_aapre[1 + CRTGT];
    static std::vector<std::vector<float>> s_aaagVis[1 + CRTGT];
    std::vector<SGSPRE>& apre = s_aapre[size_t(pv.irtgt + 1)];
    std::vector<std::vector<float>>& aagVis = s_aaagVis[size_t(pv.irtgt + 1)];
    apre.resize(size_t(items.csplit));
    aagVis.resize(size_t(items.csplit));
    // An entry's own tests (its object sphere's fade and frustum, its tree's fades into agVisTree): whether its groups
    // are looked at
    auto preOfSgs = [&](const SCENEGLOBSET& sgs, std::vector<float>& s_agVisTree, int& cmrd, int& cculled) {
        SGSPRE pre;
        if (!sgs.fTransform || sgs.irtgt != pv.irtgt) return pre;
        static const char* s_range = std::getenv("SLY2_IBRANGE");   // debugging: only globsets in [lo, hi)
        if (s_range)
        {
            size_t lo = 0, hi = 0;
            std::sscanf(s_range, "%zu,%zu", &lo, &hi);
            if (sgs.ib < lo || sgs.ib >= hi) return pre;
        }
        const GLOBSET& gs = *sgs.pglobset;
        // orig: 0x1B4478: a glob set with sphere sets (+0x48) is drawn through its volume tree (0x1D5808), not
        // RenderGlob: from set 0, each record (kind -1 skipped) whose channel mask meets CM +0x14 is tested by VU0
        // micro 0x1C0 (the four side planes by its radius, the MRD fade by its centre, x CM +0x1C); 0 culls it (a
        // kind-1 record: its whole set), a group's record submits the fade as its alpha (qw8 = gVis, 1, scale).
        // Neither the whole-object sphere nor the object's fade apply.
        const bool fTree = !s_fNoTree && pv.irtgt < 0 && !gs.asphereset.empty();
        pre.fTree = fTree;
        if (fTree)
        {
            s_agVisTree.assign(gs.aglob.size(), 0.0f);
#ifdef __SWITCH__
            // (the Switch: the groups' cel-border flags from the glob set's list (glob.cpp), while it matches them)
            const uint8_t* afCel = gs.afCelBorder.size() == gs.aglob.size() ? gs.afCelBorder.data() : nullptr;
#endif
            int aiset[256];
            int ciset = 0;
            aiset[ciset++] = 0;
            while (ciset > 0)
            {
                const int iset = aiset[--ciset];
                if (iset < 0 || size_t(iset) >= gs.asphereset.size()) continue;
                for (const GLOBSPHERE& sphere : gs.asphereset[size_t(iset)])
                {
                    if (sphere.kind == -1) continue;
                    if (!(sphere.mask & pv.grfChannels)) continue;
                    if (!s_fNoFrustum && outside(sphere.pos, sphere.sRadius)) continue;
#ifdef __SWITCH__
                    const bool fCel = sphere.kind == 0 && sphere.iglob >= 0 && size_t(sphere.iglob) < gs.aglob.size() &&
                                      (afCel ? afCel[size_t(sphere.iglob)] != 0 : gs.aglob[size_t(sphere.iglob)].meshk == MESHK_CelBorder);
#else
                    const bool fCel = sphere.kind == 0 && sphere.iglob >= 0 && size_t(sphere.iglob) < gs.aglob.size() &&
                                      gs.aglob[size_t(sphere.iglob)].meshk == MESHK_CelBorder;
#endif
                    const float gVis = UCmSphereDistanceFade(fCel ? &cmFadeCel : &cmFade, sphere.pos - cam.pos, sphere.sMRD);
                    if (gVis == 0.0f) { ++cmrd; continue; }
                    if (sphere.kind == 1) { if (ciset < 256) aiset[ciset++] = sphere.iset; }
                    else if (sphere.kind == 0 && sphere.iglob >= 0 && size_t(sphere.iglob) < gs.aglob.size())
                        s_agVisTree[size_t(sphere.iglob)] = gVis;
                }
            }
        }
        // orig: 0x1686AC..0x1687A8: the whole sphere precedes every group and supplies f20.
        float wholeFade = 1.0f;
        if (pv.irtgt < 0 && sgs.fBounds && !fTree)
        {
            wholeFade = UCmSphereDistanceFade(sgs.fMrdOriginal ? &cmFadeCel : &cmFade, sgs.posBounds - cam.pos, sgs.sMRD);
            if (sgs.oid == s_oidFade)
                std::printf("fadeobj oid %d center %.9g %.9g %.9g radius %.9g mrd %.9g adjust %.9g fade %.9g\n",
                            sgs.oid, sgs.posBounds.x, sgs.posBounds.y, sgs.posBounds.z, sgs.sBoundsRadius, sgs.sMRD, cmFade.rMRDAdjust, wholeFade);
            if (wholeFade == 0.0f) { ++cmrd; return pre; }
            if (!s_fNoFrustum && outside(sgs.posBounds, sgs.sBoundsRadius)) { ++cculled; return pre; }
        }
        pre.fDraw = true;
        pre.wholeFade = wholeFade;
        return pre;
    };
    // The records of an entry's groups [iFirst, iLim) (its tests passed: pre, s_agVisTree)
    auto recordsOfGlobs = [&](const SCENEGLOBSET& sgs, const SGSPRE& pre, const std::vector<float>& s_agVisTree, size_t iFirst, size_t iLim,
                              RECBLOCK& blk) {
        const GLOBSET& gs = *sgs.pglobset;
        const bool fTree = pre.fTree;
        const float wholeFade = pre.wholeFade;
        iLim = std::min(iLim, gs.aglob.size());
        for (size_t i = iFirst; i < iLim; ++i)
        {
#ifdef __SWITCH__
            // (the Switch: the parts of the group four ahead that the tests below read)
            if (i + 4 < iLim)
            {
                const GLOB& g = gs.aglob[i + 4];
                __builtin_prefetch(&g);
                __builtin_prefetch(&g.abone);
                __builtin_prefetch(&g.iglobTemplate);
                __builtin_prefetch(&g.pgl);
            }
#endif
            const GLOB& glob = gs.aglob[i];
            // (afVisible holds the loaded flag (0x800) as changed at run time by ShowAloGlobs: a group loaded hidden
            // and shown by a script draws; the AI code's fix for Paris' guards)
            if (i < sgs.afVisible.size() ? !sgs.afVisible[i] : glob.fHidden) continue;
            int kind = glob.rp & 0x1F;
            if (pv.irtgt >= 0)
            {
                kind = kind < 26 ? s_amapkindRtgt[pv.irtgt][kind] : -1;
                if (kind < 0) continue;
            }
            const GLOB* pgeom = &glob;
            if (!glob.pgl && glob.iglobTemplate >= 0) pgeom = &gs.aglob[size_t(glob.iglobTemplate)];
            if (!pgeom->pgl && !(pgeom->meshk == MESHK_Effect && opts.fShadows && pv.irtgt < 0)) continue;
            const glm::mat4& mat = sgs.amatGlob[i];
            const glm::vec3 posWorld = glm::vec3(MulMat4Vec4(mat, glm::vec4(glob.posCenter, 1.0f)));
            const float d = glm::distance(posWorld, cam.pos);
            static const int s_oidRec = std::getenv("SLY2_RECOID") ? std::atoi(std::getenv("SLY2_RECOID")) : -2;   // debugging
            static const int s_oidOnly = std::getenv("SLY2_ONLYOID") ? std::atoi(std::getenv("SLY2_ONLYOID")) : -2;   // debugging: only this object
            if (s_oidOnly != -2 && sgs.oid != s_oidOnly) continue;
            static const int s_oidSkip = std::getenv("SLY2_SKIPOID") ? std::atoi(std::getenv("SLY2_SKIPOID")) : -2;   // debugging: all but this object
            static const char* s_szGlobSkip = std::getenv("SLY2_SKIPGLOB");   // (with SLY2_SKIPOID: only these groups, "a,b,c")
            if (sgs.oid == s_oidSkip &&
                (!s_szGlobSkip || std::strstr((std::string(",") + s_szGlobSkip + ",").c_str(), ("," + std::to_string(i) + ",").c_str())))
                continue;
            static const int s_iglobOnly = std::getenv("SLY2_ONLYGLOB") ? std::atoi(std::getenv("SLY2_ONLYGLOB")) : -1;   // debugging: only this group (with SLY2_ONLYOID)
            if (s_iglobOnly >= 0 && s_oidOnly != -2 && int(i) != s_iglobOnly) continue;
            if (sgs.oid == s_oidRec)
                std::printf("rec oid %d #%zu rp %d d %.0f r %.1f mrd %.0f fDraw %d alpha %.2f at %.0f %.0f %.0f\n", sgs.oid, i, glob.rp, d, glob.sRadius, glob.sMRD,
                            int(glob.fDraw), sgs.gAlpha, posWorld.x, posWorld.y, posWorld.z);
            if (sgs.oid == s_oidRec && std::getenv("SLY2_RECVTX"))
            {
                std::printf("   vtx A alpha:");
                for (const VTXGLOB& v : pgeom->avtx) std::printf(" %u", v.rgbaA >> 24);
                std::printf("\n   vtx src alpha:");
                for (const VTXGLOB& v : pgeom->avtx) std::printf(" %u", v.rgbaSrc >> 24);
                std::printf("\n");
                if (glob.rp == 10)
                    for (const VTXGLOB& v : pgeom->avtx)
                    {
                        const glm::vec3 pw = glm::vec3(mat * glm::vec4(v.pos, 1.0f));
                        const glm::vec3 nw = glm::normalize(glm::mat3(mat) * v.normal);
                        const glm::vec4 c = pv.matWorldToClip * glm::vec4(pw, 1.0f);
                        const glm::vec3 tc = glm::normalize(cam.pos - pw);
                        float aSpec = float(v.rgbaA >> 24);
                        if (pgeom->iSpec == 1)
                        {
                            const glm::mat3 mInv = glm::inverse(glm::mat3(mat));
                            const glm::vec3 vv = glm::normalize(mInv * (cam.pos - glm::vec3(mat[3])) - pgeom->posSpec);
                            const float x = glm::dot(vv, glm::trunc(v.normal * 127.0f));
                            const float g = std::clamp(pgeom->agSpec[0] + pgeom->agSpec[1] * x + pgeom->agSpec[2] * x * x, pgeom->agClamp[0], pgeom->agClamp[1]);
                            aSpec = std::floor(aSpec * g + 0.5f);
                        }
                        std::printf("   gv %.1f %.1f a %.0f ndv %.4f d %.0f w %.1f %.1f %.1f n %.3f %.3f %.3f nlen %.4f\n", (c.x / c.w * 0.5f + 0.5f) * 512.0f,
                                    (0.5f - c.y / c.w * 0.5f) * 448.0f, aSpec, glm::dot(nw, tc), glm::distance(pw, cam.pos), pw.x, pw.y, pw.z, nw.x, nw.y, nw.z, glm::length(v.normal));
                    }
            }
            if (sgs.oid == s_oidRec)
                for (const SUBGLOB& sub : pgeom->asubglob)
                {
                    if (!sub.pshd) continue;
                    std::printf("   sub grf 0x%X idx %u shd %d k%d grfshd 0x%X rgba %08X saa %d:", sub.grf, sub.indexCount, sub.pshd->id, sub.pshd->shdk, sub.pshd->grfshd,
                                sub.pshd->argba[0], sub.psaa ? sub.psaa->cid : -1);
                    for (const SHDPASS& ps : sub.pshd->apass)
                        std::printf(" [tex %d itex %d col %d test 0x%llX alpha 0x%llX abe %d]", int(ps.fTextured), ps.itexPass, ps.icolor,
                                    (unsigned long long)ps.test, (unsigned long long)ps.alpha, int(ps.fBlend));
                    const TEXSEL sel = TexselFromShd(sub.pshd, sub.psaa ? sub.psaa->iframe : 0, 0);
                    if (sel.pbmp)
                    {
                        std::vector<uint8_t> rgba; std::string err;
                        if (FDecodeTexture(sel.pbmp, sel.pclut, rgba, err))
                        {
                            int ca[4] = {}; uint64_t sr = 0, sg = 0, sb = 0;
                            for (size_t k = 0; k + 3 < rgba.size(); k += 4) { ++ca[std::min(3, rgba[k + 3] / 0x40)]; sr += rgba[k]; sg += rgba[k + 1]; sb += rgba[k + 2]; }
                            const size_t n = rgba.size() / 4;
                            std::printf(" bmp %dx%d alpha q %d %d %d %d mean rgb %d %d %d", sel.pbmp->dx, sel.pbmp->dy, ca[0], ca[1], ca[2], ca[3], int(sr / n), int(sg / n), int(sb / n));
                        }
                    }
                    std::printf("\n");
                }
            if (sgs.oid == s_oidRec && !pgeom->avtx.empty())
            {
                const VTXGLOB& v = pgeom->avtx[0];
                std::printf("   vtx0 A %08X B %08X pos %.1f %.1f %.1f n %zu mat %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f bones %zu\n", v.rgbaA, v.rgbaB, v.pos.x, v.pos.y, v.pos.z, pgeom->avtx.size(), mat[0][0], mat[0][1], mat[0][2], mat[1][0], mat[1][1], mat[1][2], mat[2][0], mat[2][1], mat[2][2], i < sgs.aamatBone.size() ? sgs.aamatBone[i].size() : 0);
            }
            // orig: 0x1689CC..0x168AC4: scale the group radius; unskinned groups use the VU sphere
            // test, skinned groups use 0x14F898 without a group plane test. Neither subtracts the radius.
            const bool fSkinned = !glob.abone.empty() || !pgeom->abone.empty();
            const float radius = glob.sRadius * maxScale(mat);
            float groupFade = 1.0f;
            bool fDistance = true;
            if (fTree)
            {
                groupFade = s_agVisTree[i];
                fDistance = groupFade != 0.0f;
            }
            else if (pv.irtgt < 0 && glob.fDraw)
            {
                const CM* pcmG = pgeom->meshk == MESHK_CelBorder ? &cmFadeCel : &cmFade;
                if (fSkinned) fDistance = FInsideCmMrd(pcmG, posWorld - cam.pos, glob.sMRD, &groupFade);
                else { groupFade = UCmSphereDistanceFade(pcmG, posWorld - cam.pos, glob.sMRD); fDistance = groupFade != 0.0f; }
            }
            const float fade = s_fNoFade ? 1.0f : std::min(wholeFade, groupFade);
            if (sgs.oid == s_oidFade)
                std::printf("fadegroup oid %d group %zu center %.9g %.9g %.9g radius %.9g mrd %.9g skinned %d visible %d whole %.9g groupfade %.9g alpha %.9g alpha2 %.9g\n",
                            sgs.oid, i, posWorld.x, posWorld.y, posWorld.z, radius, glob.sMRD, int(fSkinned), int(fDistance), wholeFade,
                            groupFade, sgs.gAlpha * fade, sgs.gAlpha2 * fade);
            if (!fDistance) { ++blk.cmrd; continue; }
            const bool fGeomBounds = !fTree && !glob.fDraw && pgeom->pgl && pgeom->pgl->sRadius >= 0 && !fSkinned;
            if (pv.irtgt < 0 && !fTree && !s_fNoFrustum && pgeom->meshk != MESHK_Effect && ((glob.fDraw && !fSkinned) || fGeomBounds))
            {
                const glm::vec3 posC = fGeomBounds ? glm::vec3(MulMat4Vec4(mat, glm::vec4(pgeom->pgl->posCenter, 1.0f))) : posWorld;
                const float r = fGeomBounds ? pgeom->pgl->sRadius * maxScale(mat) : radius;
                if (outside(posC, r)) { ++blk.cculled; continue; }
            }
            DRAWREC rec;
            rec.kind = kind;
            // orig: 0x168D64..0x168D94: both input alphas receive min(wholeFade, groupFade).
            // orig: 0x169198 RenderGlobCtx: render-target draws use unit alphas without the main-view tests.
            rec.alpha = pv.irtgt >= 0 ? 1.0f : std::clamp((fTree ? 1.0f : sgs.gAlpha) * fade, 0.0f, 1.0f);
            rec.alpha2 = pv.irtgt >= 0 || fTree ? 1.0f : std::clamp(sgs.gAlpha2 * fade, 0.0f, 1.0f);
            // the foreground kinds (0x169054..0x1690E8's table by rp - 3)
            if (sgs.fForeground && pv.irtgt < 0)
                switch (kind)
                {
                case 3: case 4: kind = rec.alpha == 1.0f ? 22 : 23; break;
                case 5: case 6: kind = 23; break;
                case 9: kind = 24; break;
                case 10: kind = 25; break;
                default: break;
                }
            if (pv.irtgt < 0 && (kind >= 22 && kind <= 25) != pv.fForeground) continue;
            rec.kind = kind;
            // RenderGlob: a faded group of the opaque kinds 3 / 4 is recorded as the sorted (blended) 5 / 6
            if (rec.alpha != 1.0f && pv.irtgt < 0 && (kind == 3 || kind == 4)) rec.kind = kind + 2;
            static const char* s_kinds = std::getenv("SLY2_KINDS");   // debugging: only these kinds (hex digits list)
            if (s_kinds)
            {
                char sz[8];
                std::snprintf(sz, sizeof(sz), ",%d,", rec.kind);
                if (!std::strstr((std::string(",") + s_kinds + ",").c_str(), sz)) continue;
            }
            // orig: 0x00169100 (RenderGlob): the record's sort key is the group's z order (its header +4) when its
            // flags have 0x400 (e.g. the sky's layers, world 2 / 8), else the squared distance
            // (a group without a draw record: the volume tree's record, keyed by its sphere in the sphere sets, 0x1D5808)
            float dSort = d;
            if (!glob.fDraw && glob.fSphere) dSort = glm::distance(glm::vec3(MulMat4Vec4(mat, glm::vec4(glob.posSphere, 1.0f))), cam.pos);
            rec.z = (glob.grfGroup & 0x400) ? glob.gZOrder : dSort * dSort;
            rec.pglob = &glob;
            rec.pglobGeom = pgeom;
            rec.pmat = pmatBillboardIn(blk.amatBillboard, sgs.amatGlob[i], glob.rtck);
            rec.psgs = &sgs;
            rec.iglob = i;
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty()) rec.pamatBone = &sgs.aamatBone[i];
            blk.arec.push_back(rec);
            if (sgs.oid == s_oidFade)
                std::printf("fadesubmit oid %d group %zu kind %d alpha %.9g alpha2 %.9g\n", sgs.oid, i, rec.kind, rec.alpha, rec.alpha2);
        }
    };
    {
        // the split entries' own tests first (their parts share them)
        for (const RECITEM& item : items.aitem)
        {
            if (item.isplit < 0 || item.iglobFirst != 0) continue;
            int cmrd = 0, cculled = 0;
            apre[size_t(item.isplit)] = preOfSgs(scene.aglobset[item.isgsFirst], aagVis[size_t(item.isplit)], cmrd, cculled);
            g_renderstats.cmrd += cmrd;
            g_renderstats.cculled += cculled;
        }
        PassHelpers().ForBlocks(items.aitem.size(), 1, [&](size_t iitem, size_t) {
            const RECITEM& item = items.aitem[iitem];
            RECBLOCK& blk = ablock[iitem];
            blk.arec.clear();
            blk.amatBillboard.clear();
            blk.cmrd = blk.cculled = 0;
            if (item.isplit >= 0)
            {
                const SGSPRE& pre = apre[size_t(item.isplit)];
                if (pre.fDraw)
                    recordsOfGlobs(scene.aglobset[item.isgsFirst], pre, aagVis[size_t(item.isplit)], item.iglobFirst, item.iglobLim, blk);
                return;
            }
            thread_local std::vector<float> t_agVisTree;
            for (uint32_t isgs = item.isgsFirst; isgs < item.isgsLim; ++isgs)
            {
                const SCENEGLOBSET& sgs = scene.aglobset[isgs];
                const SGSPRE pre = preOfSgs(sgs, t_agVisTree, blk.cmrd, blk.cculled);
                if (pre.fDraw) recordsOfGlobs(sgs, pre, t_agVisTree, 0, SIZE_MAX, blk);
            }
        });
        for (size_t iitem = 0; iitem < items.aitem.size(); ++iitem)
        {
            const RECBLOCK& blk = ablock[iitem];
            arec.insert(arec.end(), blk.arec.begin(), blk.arec.end());
            g_renderstats.cmrd += blk.cmrd;
            g_renderstats.cculled += blk.cculled;
        }
    }
    if (static const bool s_fRecList = std::getenv("SLY2_RECLIST") != nullptr; s_fRecList && pv.irtgt < 0 && !pv.fForeground)   // debugging: the objects with records this frame
    {
        std::map<int, int> mp;
        const SCENEGLOBSET* psgsPrev = nullptr;
        for (const DRAWREC& r : arec) if (r.psgs != psgsPrev) { psgsPrev = r.psgs; ++mp[r.psgs->oid]; }
        for (const auto& [o, n] : mp) std::printf("reclist %d %d\n", o, n);
    }
    // extra draws (the frame only): RIPs' models at their own matrices (emitter.cpp)
    static std::vector<EXTRADRAW> s_aextra;
    s_aextra.clear();
    if (pv.irtgt < 0 && !pv.fForeground)
    {
        if (g_pfnExtraDraws) g_pfnExtraDraws(scene, s_aextra);
        CollectDrawDistanceGhosts(scene, s_aextra);   // (QoL draw distance above 1x: drawdist.h)
        CollectLaserDraws(scene, cam.pos, s_aextra);
        CollectPocketDraws(scene, s_aextra);
        CollectHackSwarmDraws(scene, s_aextra);
        // (PC: the frame's draws kept out of the depth outlines, for FBuildTranslucentMask)
        s_aextraNoOutline.clear();
        for (const EXTRADRAW& x : s_aextra)
            if (x.fNoOutline) s_aextraNoOutline.push_back(x);
        if (static const bool s_fList = std::getenv("SLY2N_EXTRALIST") != nullptr; s_fList)
            for (const EXTRADRAW& x : s_aextra)
                std::fprintf(stderr, "extradraw sgs %d glob %d at %.0f %.0f %.0f\n", x.psgs ? x.psgs->oid : -1, x.iglob, x.mat[3].x, x.mat[3].y, x.mat[3].z);
    }
    for (const EXTRADRAW& x : s_aextra)
    {
        const GLOBSET& gs = *x.psgs->pglobset;
        if (x.iglob >= gs.aglob.size()) continue;
        const GLOB& glob = gs.aglob[x.iglob];
        const GLOB* pgeom = &glob;
        if (!glob.pgl && glob.iglobTemplate >= 0) pgeom = &gs.aglob[size_t(glob.iglobTemplate)];
        if (!pgeom->pgl) continue;
        const glm::vec3 posWorld = glm::vec3(x.mat * glm::vec4(glob.posCenter, 1.0f));
        float wholeFade = 1.0f, groupFade = 1.0f;
        if (x.fBounds)
        {
            wholeFade = UCmSphereDistanceFade(&cmFade, x.posBounds - cam.pos, x.sMRD);
            if (wholeFade == 0.0f) { ++g_renderstats.cmrd; continue; }
            if (!s_fNoFrustum && outside(x.posBounds, x.sBoundsRadius)) { ++g_renderstats.cculled; continue; }
        }
        if (glob.fDraw)
        {
            const bool fSkinned = !glob.abone.empty() || !pgeom->abone.empty();
            const CM* pcmG = pgeom->meshk == MESHK_CelBorder ? &cmFadeCel : &cmFade;
            if (fSkinned)
            {
                if (!FInsideCmMrd(pcmG, posWorld - cam.pos, glob.sMRD, &groupFade)) { ++g_renderstats.cmrd; continue; }
            }
            else
            {
                groupFade = UCmSphereDistanceFade(pcmG, posWorld - cam.pos, glob.sMRD);
                if (groupFade == 0.0f) { ++g_renderstats.cmrd; continue; }
                if (!s_fNoFrustum && outside(posWorld, glob.sRadius * maxScale(x.mat))) { ++g_renderstats.cculled; continue; }
            }
        }
        const float alpha = x.alpha * (s_fNoFade ? 1.0f : std::min(wholeFade, groupFade));
        DRAWREC rec;
        rec.kind = glob.rp & 0x1F;
        const float d = glm::distance(posWorld, cam.pos);
        rec.z = (glob.grfGroup & 0x400) ? glob.gZOrder : d * d;  // 0x169100 (as above)
        rec.pglob = &glob;
        rec.pglobGeom = pgeom;
        rec.pmat = pmatBillboard(x.mat, glob.rtck);
        rec.alpha = rec.alpha2 = std::clamp(alpha, 0.0f, 1.0f);
        if (rec.alpha != 1.0f && (rec.kind == 3 || rec.kind == 4)) rec.kind += 2;
        arec.push_back(rec);
        if (x.psgs->oid == s_oidFade)
            std::printf("fadesubmitextra oid %d group %zu kind %d alpha %.9g alpha2 %.9g\n", x.psgs->oid, x.iglob, rec.kind, rec.alpha, rec.alpha2);
    }
    if (pv.irtgt < 0 && [] { static const bool f = std::getenv("SLY2_PICK") != nullptr; return f; }())
    {
        glm::vec3 dir = cam.Dir();
        // SLY2_PICK="sx sy" (-1..1, y up): the ray through that screen point instead of the centre
        float sx = 0.0f, sy = 0.0f;
        if (std::sscanf(std::getenv("SLY2_PICK"), "%f %f", &sx, &sy) == 2)
        {
            const glm::mat4 inv = glm::inverse(matWorldToClip);
            const glm::vec4 a = inv * glm::vec4(sx, sy, 0.5f, 1.0f);
            dir = glm::normalize(glm::vec3(a) / a.w - cam.pos);
            std::printf("pick ray %.3f %.3f %.3f from %.0f %.0f %.0f\n", dir.x, dir.y, dir.z, cam.pos.x, cam.pos.y, cam.pos.z);
        }
        for (const SCENEGLOBSET& sgs : scene.aglobset)
        {
            if (!sgs.fTransform) continue;
            if (const char* r = std::getenv("SLY2_IBRANGE"))
            {
                size_t lo = 0, hi = 0;
                std::sscanf(r, "%zu,%zu", &lo, &hi);
                if (sgs.ib < lo || sgs.ib >= hi) continue;
                std::printf("in range: glob@%zu va 0x%X groups %zu\n", sgs.ib, sgs.vaGlob, sgs.pglobset->aglob.size());
                for (size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f));
                    std::printf("  grfMesh 0x%X cbone %d gZ %g\n", g.grfMesh, g.cbone, g.gZOrder);
                    for (const SUBGLOB& sub : g.asubglob)
                        if (sub.pshd)
                            for (int tp = 0; tp < 2; ++tp)
                            {
                                const TEXSEL sel = TexselFromShd(sub.pshd, 0, tp);
                                if (!sel.pbmp) continue;
                                std::vector<uint8_t> rgba; std::string err;
                                if (!FDecodeTexture(sel.pbmp, sel.pclut, rgba, err)) continue;
                                int ca[4] = {};
                                for (size_t k = 3; k < rgba.size(); k += 4) ++ca[std::min(3, rgba[k] / 0x40)];
                                std::printf("    shd 0x%X pass %d bmp 0x%X %dx%d alpha quartiles %d %d %d %d\n", sub.pshd->id, tp, sel.pbmp->id, sel.pbmp->dx, sel.pbmp->dy, ca[0], ca[1], ca[2], ca[3]);
                            }
                    std::printf("  #%zu rp %d meshk %d r %.0f at %.0f %.0f %.0f hidden %d shdk %d grfshd 0x%X A %08X\n", i, g.rp, g.meshk, g.sRadius, c.x, c.y, c.z,
                                int(g.fHidden || !sgs.afVisible[i]), g.asubglob.empty() || !g.asubglob[0].pshd ? -1 : g.asubglob[0].pshd->shdk,
                                g.asubglob.empty() || !g.asubglob[0].pshd ? 0 : g.asubglob[0].pshd->grfshd, g.avtx.empty() ? 0 : g.avtx[0].rgbaA);
                }
            }
            for (size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
            {
                const GLOB& glob = sgs.pglobset->aglob[i];
                const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(glob.posCenter, 1.0f));
                const float t = glm::dot(c - cam.pos, dir);
                const float dperp = glm::length(c - cam.pos - dir * t);
                const GLOB& gr = !glob.pgl && glob.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(glob.iglobTemplate)] : glob;
                const float sRPick = std::max(glob.sRadius, gr.pgl ? gr.pgl->sRadius : 0.0f);
                if (t > 0 && dperp < sRPick)
                    std::printf("pick: oid %d #%zu rp %d meshk %d r %.0f t %.0f hidden %d shd0 0x%X c %.0f %.0f %.0f fog %g z %g grf 0x%X A %08X\n", sgs.oid, i,
                                glob.rp, glob.meshk, sRPick, t, int(glob.fHidden || !sgs.afVisible[i]),
                                glob.asubglob.empty() || !glob.asubglob[0].pshd ? 0 : glob.asubglob[0].pshd->id, c.x, c.y, c.z, glob.uFog, glob.gZOrder,
                                glob.grfGroup, gr.avtx.empty() ? 0 : gr.avtx[0].rgbaA);
                if (glob.asubglob.size() && glob.asubglob[0].pshd && t > 0 && dperp < sRPick)
                {
                    const GLOB& g = glob.avtx.empty() && glob.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(glob.iglobTemplate)] : glob;
                    if (!g.avtx.empty())
                        std::printf("      shdk %d grfshd 0x%X grf 0x%X si %d A %08X B %08X unbaked %d inst %d\n", glob.asubglob[0].pshd->shdk,
                                    glob.asubglob[0].pshd->grfshd, glob.asubglob[0].grf, glob.asubglob[0].pshd->unSelfIllum,
                                    g.avtx[0].rgbaA, g.avtx[0].rgbaB, g.avtx[0].fUnbaked, glob.iglobTemplate);
                }
            }
        }
    }
    // DrawRpl 0x157FA0 / SortRpl 0x157AE0: by kind; kinds 1, 2, 5, 6, 10, 25 far to near, 21 near to far.
    auto lessRec = [](const DRAWREC& a, const DRAWREC& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        switch (a.kind)
        {
        case 1: case 2: case 5: case 6: case 10: case 25: return a.z > b.z;
        case 21: return a.z < b.z;
        default: return false;
        }
    };
#ifdef __SWITCH__
    // (the Switch: the keys sorted, then the records moved once. A key is the kind, the distance's bits in float order
    // (-0 as +0; reversed for far to near) and the place: ties keep their order as in the stable sort. NaN: the sort.)
    constexpr unsigned CBIT_PLACE = 27;
    static std::vector<uint64_t> s_akeyRec;
    static std::vector<DRAWREC> s_arecSorted;
    bool fKeys = arec.size() < (size_t(1) << CBIT_PLACE);
    s_akeyRec.clear();
    for (size_t i = 0; i < arec.size() && fKeys; ++i)
    {
        const DRAWREC& r = arec[i];
        uint32_t zkey = 0;
        if (r.kind < 0 || r.kind > 31) fKeys = false;
        else if (r.kind == 1 || r.kind == 2 || r.kind == 5 || r.kind == 6 || r.kind == 10 || r.kind == 25 || r.kind == 21)
        {
            if (std::isnan(r.z)) fKeys = false;
            const float z = r.z == 0.0f ? 0.0f : r.z;
            uint32_t u;
            std::memcpy(&u, &z, 4);
            u = (u & 0x80000000u) ? ~u : (u | 0x80000000u);
            zkey = r.kind == 21 ? u : ~u;
        }
        s_akeyRec.push_back(uint64_t(uint32_t(r.kind)) << 59 | uint64_t(zkey) << CBIT_PLACE | uint64_t(i));
    }
    if (fKeys)
    {
        std::sort(s_akeyRec.begin(), s_akeyRec.end());
        s_arecSorted.resize(arec.size());
        for (size_t i = 0; i < s_akeyRec.size(); ++i) s_arecSorted[i] = arec[size_t(s_akeyRec[i] & ((uint64_t(1) << CBIT_PLACE) - 1))];
        arec.swap(s_arecSorted);
    }
    else std::stable_sort(arec.begin(), arec.end(), lessRec);
#else
    std::stable_sort(arec.begin(), arec.end(), lessRec);
#endif

    // The frame's draw stream: per record its matrix / fog (RECGPU), per entry its draw data (DRAWGPU), per pass
    // one or two commands with their GL state; volumes, shadows and cel borders are special commands.
    static std::vector<RECGPU> s_arecgpu;
    static std::vector<DRAWGPU> s_adrawgpu;
    static std::vector<CMD> s_acmd;
    static std::vector<glm::mat4> s_amatBone;
    s_amatBone.clear();
    for (DRAWREC& rec : arec)
        if (rec.pamatBone)
        {
            rec.iboneFirst = uint32_t(s_amatBone.size());
            s_amatBone.insert(s_amatBone.end(), rec.pamatBone->begin(), rec.pamatBone->end());
        }
    s_arecgpu.clear();
    s_adrawgpu.clear();
    s_acmd.clear();
    // the warped globs' state vectors and their WRs' matrices (wr.h; glob.vert adds sum(matrix x state))
    static std::vector<glm::vec4> s_awarpst;
    static std::vector<glm::mat4> s_amatWarp;
    static std::unordered_map<const WRBG*, uint32_t> s_mpwrbgist;
    static std::unordered_map<const WR*, uint32_t> s_mpwrimat;
    static const bool s_fNoWarp = std::getenv("SLY2_NOWARP") != nullptr;   // debugging: no space warps
    s_awarpst.clear();
    s_amatWarp.clear();
    s_mpwrbgist.clear();
    s_mpwrimat.clear();
    auto warpOf = [&](const GLOB& geom) {
        const WRBG* pw = geom.pwrbg.get();
        const WR* pwr = PwrOfWrbg(pw);
        if (!pwr || s_fNoWarp || !(pw->fDpos || pw->fDuv)) return glm::uvec4(0u);
        const size_t cvtx = geom.avtx.size();
        const uint32_t cmat = uint32_t(std::clamp(pw->cmat, 0, 4));
        if (cmat == 0 || pw->astate.size() < cmat * cvtx) return glm::uvec4(0u);
        auto [itst, fNewSt] = s_mpwrbgist.try_emplace(pw, uint32_t(s_awarpst.size()) + 1u);
        if (fNewSt) s_awarpst.insert(s_awarpst.end(), pw->astate.begin(), pw->astate.begin() + std::ptrdiff_t(cmat * cvtx));
        auto [itmat, fNewMat] = s_mpwrimat.try_emplace(pwr, uint32_t(s_amatWarp.size()));
        if (fNewMat)
        {
            s_amatWarp.insert(s_amatWarp.end(), std::begin(pwr->amatDpos), std::end(pwr->amatDpos));
            s_amatWarp.insert(s_amatWarp.end(), std::begin(pwr->amatDuv), std::end(pwr->amatDuv));
        }
        return glm::uvec4(itst->second, uint32_t(cvtx), itmat->second, cmat | (pw->fDpos ? 0x100u : 0u) | (pw->fDuv ? 0x200u : 0u));
    };
    s_avtxShadow2.clear();
    // Kinds drawn in record order with no sort (DrawRpl): their commands are regrouped by pass and state, so
    // the runs are long; the sorted (blended) kinds keep their order, merging only neighbours.
    static const bool s_fNoRegroup = std::getenv("SLY2_NOREGROUP") != nullptr;
    auto fSortedKind = [](int kind) {
        switch (kind)
        {
        case 1: case 2: case 5: case 6: case 10: case 25: case 21: return true;
        default: return false;
        }
    };
    struct SEGMENT { size_t icmdFirst, icmdLim; bool fRegroup; };
    std::vector<SEGMENT> aseg;
    size_t icmdSeg = 0;
    int kindSeg = -1;
    auto endSegment = [&](bool fRegroup) {
        if (s_acmd.size() > icmdSeg) aseg.push_back({icmdSeg, s_acmd.size(), fRegroup});
        icmdSeg = s_acmd.size();
    };
    static const char* s_cull = std::getenv("SLY2_CULL");   // "none": no back-face culling
    const bool fCullNone = s_cull && !std::strcmp(s_cull, "none");
    // the particles (render kind 12, emitter.cpp) where the frame draws that kind
    bool fParticles = pv.irtgt >= 0 || pv.fForeground;
    auto addParticles = [&]() {
        if (fParticles) return;
        fParticles = true;
        endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
        CMD cmd;
        cmd.cmdk = CMDK_Particles;
        s_acmd.push_back(cmd);
        endSegment(false);
    };
    // The type-2 shadows (render kind 7: the characters' cast shadows) mark with the kind-7 hulls; the frame's alpha
    // keeps the characters' own pixels out (DATE), as on the PS2.
    bool fShadow2 = false;
    auto addShadow2 = [&]() {
        if (fShadow2 || !opts.fShadows || pv.fForeground) return;
        fShadow2 = true;
#if !defined(__SWITCH__) && !defined(__APPLE__)
        // (PC TEST, character shadow maps: the frame's pass only; the casters' lights, nearest the camera first, and one
        // command that draws their maps and the shadows in place of the volumes)
        if (g_display.shadowMaps && pv.irtgt < 0 && FShadowMapsReady())
        {
            s_asmcaster.clear();
            std::vector<std::pair<const SCENEGLOBSET*, std::vector<glm::vec3>>> agroup;
            for (const DRAWREC& rec : arec)
            {
                const GLOB& geom = *rec.pglobGeom;
                if (!(geom.meshk == MESHK_Effect && rec.psgs && !geom.asubfx.empty() && geom.asubfx[0].pshd)) continue;
                auto it = std::find_if(agroup.begin(), agroup.end(), [&](const auto& g) { return g.first == rec.psgs; });
                if (it == agroup.end()) { agroup.push_back({rec.psgs, {}}); it = agroup.end() - 1; }
                SkinShadowHulls(*rec.psgs, rec.iglob, geom, it->second);
            }
            for (const auto& g : agroup)
            {
                SMCASTER c;
                if (FSmCasterOf(*g.first, g.second, cam.pos, &c)) s_asmcaster.push_back(c);
            }
            std::sort(s_asmcaster.begin(), s_asmcaster.end(), [](const SMCASTER& a, const SMCASTER& b) { return a.distCamera < b.distCamera; });
            if (s_asmcaster.size() > size_t(kcSmCasterMax)) s_asmcaster.resize(size_t(kcSmCasterMax));
            if (s_asmcaster.empty()) return;
            endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
            CMD cmd;
            cmd.cmdk = CMDK_ShadowMap;
            s_acmd.push_back(cmd);
            endSegment(false);
            return;
        }
#endif
        // all the volumes in one count, marked once (their union: a character's overlapping pieces, as the PS2's
        // green count, darken the floor once)
        CMD cmdAll;
        cmdAll.firstIndex = uint32_t(s_avtxShadow2.size());
        cmdAll.count = 0;
        cmdAll.idraw = 0;
        cmdAll.order = 8;
        auto fCaster = [](const DRAWREC& rec) {
            const GLOB& geom = *rec.pglobGeom;
            return geom.meshk == MESHK_Effect && rec.psgs && !geom.asubfx.empty() && geom.asubfx[0].pshd;
        };
        auto addCaster = [&](const DRAWREC& rec, const CMD& cmd) {
            cmdAll.count += cmd.count;
            cmdAll.idraw = std::max(cmdAll.idraw, cmd.idraw);
            if (!(rec.pglobGeom->asubfx[0].pshd->grfshd & 8)) cmdAll.order = 0;   // (DATE unless every shader has flag 8)
            cmdAll.prec = &rec;
        };
#ifdef __SWITCH__
        // (the Switch: the volumes built on the frame's helpers, each caster's into its own vertices, joined in order)
        struct CASTER { const DRAWREC* prec = nullptr; std::vector<glm::vec3> avtx; CMD cmd; bool f = false; };
        static std::vector<CASTER> s_acaster;
        size_t ccaster = 0;
        for (const DRAWREC& rec : arec)
            if (fCaster(rec))
            {
                if (ccaster == s_acaster.size()) s_acaster.emplace_back();
                s_acaster[ccaster++].prec = &rec;
            }
        PassHelpers().ForBlocks(ccaster, 1, [&](size_t iFirst, size_t iLim) {
            for (size_t i = iFirst; i < iLim; ++i)
            {
                CASTER& c = s_acaster[i];
                c.avtx.clear();
                c.cmd = CMD{};
                c.f = FBuildShadow2(*c.prec->psgs, c.prec->iglob, *c.prec->pglobGeom, cam.pos, 52.0f, c.avtx, c.cmd);
            }
        });
        s_acvtxShadow2Caster.clear();
        for (size_t i = 0; i < ccaster; ++i)
        {
            const CASTER& c = s_acaster[i];
            if (!c.f) continue;
            s_avtxShadow2.insert(s_avtxShadow2.end(), c.avtx.begin(), c.avtx.end());
            s_acvtxShadow2Caster.push_back(uint32_t(c.avtx.size()));
            addCaster(*c.prec, c.cmd);
        }
#else
        for (const DRAWREC& rec : arec)
        {
            if (!fCaster(rec)) continue;
            CMD cmd;
            if (!FBuildShadow2(*rec.psgs, rec.iglob, *rec.pglobGeom, cam.pos, 52.0f, s_avtxShadow2, cmd)) continue;
            addCaster(rec, cmd);
        }
#endif
        if (cmdAll.count == 0) return;
        endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
        cmdAll.cmdk = CMDK_Shadow2;
        s_acmd.push_back(cmdAll);
        endSegment(false);
    };
    // A shadow phase: kinds 7 / 8 between 0x16DA48 (begin: the frame's alpha kept) and 0x16DD58 (combine: the marked
    // pixels darkened). count: bit 0 kind 7 drawn, bit 1 kind 8.
    const bool fFbaPass = pv.irtgt < 0 && !pv.fForeground;
    int icmdShPhase = -1;
    bool fFbaOpen = true;   // (the FBA kinds' stencil bits not cleared yet)
    auto shadowCmd = [&](CMDK cmdk) {
        endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
        CMD cmd;
        cmd.cmdk = cmdk;
        s_acmd.push_back(cmd);
        endSegment(false);
    };
    auto shadowPhase = [&](int kind) {
        const bool fShKind = opts.fShadows && !pv.fForeground && (kind == 7 || kind == 8);
        if (fFbaOpen && (kind > 6 || kind < 0) && !fShKind)
        {
            shadowCmd(CMDK_FbaClear);
            fFbaOpen = false;
        }
        if (fShKind) fFbaOpen = false;   // (the combine clears them)
        if (icmdShPhase >= 0 && !fShKind)
        {
            shadowCmd(CMDK_ShadowCombine);
            s_acmd.back().count = s_acmd[size_t(icmdShPhase)].count;
            icmdShPhase = -1;
        }
        else if (fShKind && icmdShPhase < 0)
        {
            shadowCmd(CMDK_ShadowBegin);
            icmdShPhase = int(s_acmd.size()) - 1;
            s_acmd.back().order = uint32_t(kind);
        }
        else if (fShKind && kind == 8) shadowCmd(CMDK_ShadowKind8);
    };
    for (const DRAWREC& rec : arec)
    {
#ifdef __SWITCH__
        PrefetchRecordsAhead(arec, size_t(&rec - arec.data()));
#endif
        const GLOB& glob = *rec.pglob;
        const GLOB& geom = *rec.pglobGeom;
        if (rec.kind != kindSeg) shadowPhase(rec.kind);
        if (rec.kind == 7 && icmdShPhase >= 0)
        {
            const size_t ccmd = s_acmd.size();
            addShadow2();
            if (s_acmd.size() != ccmd) s_acmd[size_t(icmdShPhase)].count |= 1;
        }
        if (rec.kind > 12) addParticles();
        if (rec.kind != kindSeg) { endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg)); kindSeg = rec.kind; }
        if (s_adrawgpu.size() + geom.asubglob.size() * (g_fGlobBindless ? 16 : 1) + 1 > CDRAW_MAX) break;
        const uint32_t irec = uint32_t(s_arecgpu.size());
        if (geom.meshk == MESHK_Strips && opts.fMeshes)
        {
            // an instance lit at its own place draws its own copy of the vertices (SCENEGLOBSET::apglInst)
            const GLGLOB* pglDraw = rec.psgs && rec.iglob < rec.psgs->apglInst.size() && rec.psgs->apglInst[rec.iglob] &&
                                            rec.psgs->apglInst[rec.iglob]->vao
                                        ? rec.psgs->apglInst[rec.iglob]
                                        : geom.pgl;
            // CM_BuildFogRecip(g_cm, qw 9, uFog): uFog unset (FLT_MAX) counts as 1.
            const float uFog = glob.uFog == std::numeric_limits<float>::max() ? 1.0f : glob.uFog;
            s_arecgpu.push_back({*rec.pmat, glm::vec4(opts.sFogNear, opts.sFogFar / std::max(opts.sFogNear, 1.0f),
                                                      opts.uFogMax * uFog, opts.fFog ? 1.0f : 0.0f),
                                 glm::uvec4(rec.iboneFirst, rec.pamatBone ? uint32_t(rec.pamatBone->size()) : 0u, uint32_t(pglDraw->baseVertex), 0u)});
            s_arecgpu.back().warp = warpOf(geom);
            // orig: P_G 638 (0x16A000 SPECPARAMS): the view-angle alpha. Unlit groups only: a lit group's gain scales its
            // normals' brightness (w) for the lighting (522) instead (TODO(render): not modelled)
            if (geom.iSpec != -1 && !(geom.grfMesh & 1))
            {
                RECGPU& r = s_arecgpu.back();
                r.spec0 = glm::vec4(geom.agSpec[0], geom.agSpec[1], geom.agSpec[2], float(geom.iSpec));
                r.spec1 = glm::vec4(geom.agClamp[0], geom.agClamp[1], 1.0f, 0.0f);
                r.specPos = glm::vec4(geom.posSpec, 0.0f);
            }
            CMD base;
            base.vao = pglDraw->vao;
            // the bound of the record's vertex alpha x object alpha, for the alpha tests it passes on every fragment
            // (AllPassOf); 0: not decided (the view-angle alpha changes it per vertex)
            const bool fSpec = geom.iSpec != -1 && !(geom.grfMesh & 1);
            const float vMin = fSpec ? 0.0f : float(pglDraw->aMinA) * rec.alpha;
            // P_G culls back faces unless the mesh is double-sided (flag 0x10 -> param 0x80).
            const uint8_t fCull = !(geom.grfMesh & 0x10) && !fCullNone;
            base.baseVertex = pglDraw->baseVertex;
            base.prec = &rec;
            for (const SUBGLOB& sub : geom.asubglob)
            {
                if (!sub.pshd || sub.indexCount == 0) continue;
                // (debug: SLY2N_SKIPSHD=id: that shader's entries not drawn)
                if (static const int s_shdSkip = std::getenv("SLY2N_SKIPSHD") ? std::atoi(std::getenv("SLY2N_SKIPSHD")) : -2; sub.pshd->id == s_shdSkip) continue;
                ++g_renderstats.cdraw;
                base.idraw = uint32_t(s_adrawgpu.size());
                base.count = sub.indexCount;
                base.firstIndex = pglDraw->firstIndex + sub.firstIndex;
                base.psub = &sub;
                if (sub.pshd->shdk == 3)
                {
                    // orig: 0x0016A000 (one projected-volume batch can have several entries of the same material).
                    // Keep that hull together through all mask/fill/clear passes. The entries can be open pieces
                    // of one closed hull: clearing the mask after each piece lights surfaces outside the volume.
                    if (!s_acmd.empty())
                    {
                        CMD& prev = s_acmd.back();
                        if (prev.cmdk == CMDK_Volume && prev.prec == &rec && prev.psub->pshd == sub.pshd &&
                            prev.psub->psaa == sub.psaa && prev.psub->ibatch == sub.ibatch &&
                            prev.vao == base.vao && prev.baseVertex == base.baseVertex &&
                            prev.firstIndex + prev.count == base.firstIndex)
                        {
                            prev.count += base.count;
                            continue;
                        }
                    }
                    s_adrawgpu.push_back({0.0f, 0.0f, 1.0f, irec});
                    endSegment(!s_fNoRegroup && !fSortedKind(rec.kind));
                    CMD cmd = base;
                    cmd.cmdk = CMDK_Volume;
                    s_acmd.push_back(cmd);
                    endSegment(false);
                    continue;
                }
                const DRAWGPU drawgpu{sub.psaa ? sub.psaa->du / 4096.0f : 0.0f, sub.psaa ? sub.psaa->dv / 4096.0f : 0.0f, rec.alpha, irec};
                if (!g_fGlobBindless) s_adrawgpu.push_back(drawgpu);
                for (const PASSCMD& pc : SubgpuOf(sub, pglDraw->vao, fCull).apasscmd)
                {
                    uint32_t istate = pc.istate;
                    if (vMin > 0.0f && s_astate[istate].ate)
                    {
                        const ALLPASS ap = AllPassOf(istate);
                        if (vMin >= ap.vNeed)
                        {
                            if (ap.istateAlt == UINT32_MAX) continue;   // (the AFAIL sub-draw: keeps nothing)
                            istate = ap.istateAlt;
                        }
                    }
                    base.istate = fFbaPass && (rec.kind == 4 || rec.kind == 6) ? IstateFba(istate) : istate;
                    base.order = pc.order;
                    // (bindless: each pass draw its own draw entry, for its texture handle: s_atexh)
                    if (g_fGlobBindless)
                    {
                        base.idraw = uint32_t(s_adrawgpu.size());
                        s_adrawgpu.push_back(drawgpu);
                    }
                    s_acmd.push_back(base);
                }
            }
        }
        else if (geom.meshk == MESHK_Indexed && opts.fShadows && geom.pgl->cindex > 0 && !geom.asubfx.empty() && geom.asubfx[0].pshd)
        {
            s_arecgpu.push_back({*rec.pmat, glm::vec4(1, 1, 0, 0)});
            endSegment(!s_fNoRegroup && !fSortedKind(rec.kind));
            CMD cmd;
            cmd.cmdk = CMDK_Shadow;
            if (icmdShPhase >= 0) s_acmd[size_t(icmdShPhase)].count |= rec.kind == 8 ? 2u : 1u;
            cmd.vao = geom.pgl->vao;
            cmd.count = uint32_t(geom.pgl->cindex);
            cmd.firstIndex = geom.pgl->firstIndex;
            cmd.baseVertex = geom.pgl->baseVertex;
            cmd.idraw = uint32_t(s_adrawgpu.size());
            cmd.prec = &rec;
            s_adrawgpu.push_back({0.0f, 0.0f, 1.0f, irec});
            s_acmd.push_back(cmd);
            endSegment(false);
        }
        else if (geom.meshk == MESHK_CelBorder && opts.fCelBorders && geom.pgl && geom.pgl->cedge > 0)
        {
            endSegment(!s_fNoRegroup && !fSortedKind(rec.kind));
            CMD cmd;
            cmd.cmdk = CMDK_Cel;
            cmd.prec = &rec;
            s_acmd.push_back(cmd);
            endSegment(false);
        }
    }
    shadowPhase(-1);
    addParticles();
    endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
    // Regroup: within a segment of an unsorted kind, by (pass, sub-draw) then state (stable: record order inside).
    static std::vector<uint64_t> s_akey;
    static std::vector<CMD> s_acmdTmp;
    for (const SEGMENT& seg : aseg)
    {
        if (!seg.fRegroup || seg.icmdLim - seg.icmdFirst < 2) continue;
        s_akey.clear();
        for (size_t i = seg.icmdFirst; i < seg.icmdLim; ++i)
        {
            const CMD& c = s_acmd[i];
            s_akey.push_back((uint64_t(std::min(c.order, 0xFFFu)) << 52) | (uint64_t(c.istate & 0xFFFFFFu) << 28) | uint64_t(i - seg.icmdFirst));
        }
        SortKeysUnique(s_akey);
        s_acmdTmp.assign(s_acmd.begin() + std::ptrdiff_t(seg.icmdFirst), s_acmd.begin() + std::ptrdiff_t(seg.icmdLim));
        for (size_t i = 0; i < s_akey.size(); ++i) s_acmd[seg.icmdFirst + i] = s_acmdTmp[size_t(s_akey[i] & 0xFFFFFFFu)];
    }
    // Runs of equal state -> indirect commands.
    struct RUN { size_t icmd; uint32_t ideiFirst, cdei; };
    static std::vector<DEICMD> s_adei;
    static std::vector<RUN> s_arun;
    s_adei.clear();
    s_arun.clear();
    for (size_t i = 0; i < s_acmd.size();)
    {
        const CMD& cmd = s_acmd[i];
        if (cmd.cmdk != CMDK_Pass) { s_arun.push_back({i, 0, 0}); ++i; continue; }
        RUN run{i, uint32_t(s_adei.size()), 0};
        size_t j = i;
        // (bindless: the neighbours that differ only in their textures join the run, in the same order)
        for (; j < s_acmd.size() && s_acmd[j].cmdk == CMDK_Pass && (s_acmd[j].istate == cmd.istate || (g_fGlobBindless && FSameButTexture(cmd.istate, s_acmd[j].istate))); ++j)
        {
            const CMD& c = s_acmd[j];
            s_adei.push_back({c.count, 1, c.firstIndex, c.baseVertex, c.idraw});
            ++run.cdei;
        }
        s_arun.push_back(run);
        i = j;
    }
    static const bool s_fPerfDbg = std::getenv("SLY2_PERFDBG") != nullptr;
    if (s_fPerfDbg)
    {
        // records / commands / runs per kind, and records without a draw record (never culled)
        int acrec[32] = {}, acmd[32] = {}, acrun[32] = {}, acnodraw[32] = {};
        for (const DRAWREC& rec : arec) { ++acrec[rec.kind]; if (!rec.pglob->fDraw) ++acnodraw[rec.kind]; }
        for (const CMD& c : s_acmd) if (c.prec) ++acmd[c.prec->kind];
        for (const RUN& r : s_arun) if (s_acmd[r.icmd].prec) ++acrun[s_acmd[r.icmd].prec->kind];
        for (int k = 0; k < 32; ++k)
            if (acrec[k]) std::printf("kind %2d: %5d records (%d without bounds), %5d commands, %5d runs\n", k, acrec[k], acnodraw[k], acmd[k], acrun[k]);
    }
    // (each array's place: in the frame's ring segment when the pass fits (all or none), else its own orphaned buffer)
    struct BUFRANGE { GLuint buf; size_t off, cb; };
    const size_t cbAll = s_arecgpu.size() * sizeof(RECGPU) + s_amatBone.size() * sizeof(glm::mat4) + s_adrawgpu.size() * sizeof(DRAWGPU) +
                         s_awarpst.size() * sizeof(glm::vec4) + s_amatWarp.size() * sizeof(glm::mat4) + s_adei.size() * sizeof(DEICMD) +
                         (g_fGlobBindless ? s_adrawgpu.size() * sizeof(GLuint64) : 0) + 8 * (s_ring.align + 16);
    size_t offRing = s_ring.Alloc(cbAll);
    auto upload = [&](GLenum target, GLuint buf, const void* pv, size_t cb) -> BUFRANGE {
        const size_t cbBind = std::max<size_t>(cb, 16);
        if (offRing != SIZE_MAX)
        {
            const size_t o = (offRing + s_ring.align - 1) / s_ring.align * s_ring.align;
            if (cb) std::memcpy(s_ring.pb + o, pv, cb);
            offRing = o + cbBind;
            return {s_ring.buf, o, cbBind};
        }
        glBindBuffer(target, buf);
        glBufferData(target, GLsizeiptr(cbBind), nullptr, GL_STREAM_DRAW);   // orphan
        if (cb) glBufferSubData(target, 0, GLsizeiptr(cb), pv);
        return {buf, 0, cbBind};
    };
    const BUFRANGE brRec = upload(GL_SHADER_STORAGE_BUFFER, s_ssboRec, s_arecgpu.data(), s_arecgpu.size() * sizeof(RECGPU));
    const BUFRANGE brBone = upload(GL_SHADER_STORAGE_BUFFER, s_ssboBone, s_amatBone.data(), s_amatBone.size() * sizeof(glm::mat4));
    const BUFRANGE brDraw = upload(GL_SHADER_STORAGE_BUFFER, s_ssboDraw, s_adrawgpu.data(), s_adrawgpu.size() * sizeof(DRAWGPU));
    const BUFRANGE brWarp = upload(GL_SHADER_STORAGE_BUFFER, s_ssboWarp, s_awarpst.data(), s_awarpst.size() * sizeof(glm::vec4));
    const BUFRANGE brWarpMat = upload(GL_SHADER_STORAGE_BUFFER, s_ssboWarpMat, s_amatWarp.data(), s_amatWarp.size() * sizeof(glm::mat4));
    const BUFRANGE brIndirect = upload(GL_DRAW_INDIRECT_BUFFER, s_bufIndirect, s_adei.data(), s_adei.size() * sizeof(DEICMD));
    BUFRANGE brTexh{0, 0, 0};
    if (g_fGlobBindless)
    {
        // each pass draw's texture handle by its draw entry (0: the other entries, and a state without one)
        static std::vector<GLuint64> s_atexh;
        s_atexh.assign(s_adrawgpu.size(), 0);
        for (const CMD& c : s_acmd)
            if (c.cmdk == CMDK_Pass) s_atexh[c.idraw] = HandleOfIstate(c.istate);
        brTexh = upload(GL_SHADER_STORAGE_BUFFER, s_ssboTexh, s_atexh.data(), s_atexh.size() * sizeof(GLuint64));
    }

    glBindFramebuffer(GL_FRAMEBUFFER, pv.fbo);
    glViewport(0, 0, pv.dx, pv.dy);
    const glm::vec4 clear = Vec4FromRgba(pv.rgbaClear, 1.0f / 255.0f);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(clear.r, clear.g, clear.b, float(pv.alphaClear) / 128.0f);
    glClearDepth(0.0);
    glStencilMask(0xFF);
    glClearStencil(0);
    // (the foreground pass: only the depth (0x2DF010's Z 0 over the frame) and the stencil, over the frame and HUD)
    glClear(pv.fForeground ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_CULL_FACE);

    auto bindRange = [](GLuint ibinding, const BUFRANGE& br) {
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, ibinding, br.buf, GLintptr(br.off), GLsizeiptr(br.cb));
    };
    bindRange(0, brRec);
    bindRange(1, brDraw);
    bindRange(2, brBone);
    bindRange(3, brWarp);
    bindRange(4, brWarpMat);
    if (g_fGlobBindless) bindRange(5, brTexh);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, brIndirect.buf);
    const size_t offIndirect = brIndirect.off;
    glActiveTexture(GL_TEXTURE0);
    STATECACHE sc;
    // the glob program bound (0 glGlobShader, 1 glGlobShaderNoKill, -1 another program), and which of the two have this
    // pass's frame uniforms
    int iprogGlob = -1;
    bool afFrameUniforms[2] = {};
    auto useGlob = [&](int iprog) {
        if (iprog == 1 && !glGlobShaderNoKill.ID) iprog = 0;
        if (iprog == iprogGlob) return;
        const bool fFromOther = iprogGlob < 0;
        iprogGlob = iprog;
        s_iprogGlob = iprog;
        s_pu = &s_au[iprog];
        sc.iprog = iprog;
        s_apshGlob[iprog]->Use();
        if (!afFrameUniforms[iprog])
        {
            glUniformMatrix4fv(s_pu->matWorldToClip, 1, GL_FALSE, glm::value_ptr(matWorldToClip));
            glUniform3fv(s_pu->posCamera, 1, glm::value_ptr(cam.pos));
            glUniform3fv(s_pu->dirCamera, 1, glm::value_ptr(cam.Dir()));
            glUniform1i(s_pu->tex, 0);
            glUniform1i(s_pu->texDate, 1);
            glUniform1i(s_pu->fDate, 0);
            glUniform4fv(s_pu->fogColor, 1, glm::value_ptr(Vec4FromRgba(opts.rgbaFog, 1.0f / 255.0f)));
            afFrameUniforms[iprog] = true;
        }
        SyncGlobInherited(sc);
        if (fFromOther) sc.fValid = false;   // (the GL state another program's draws changed; uniforms are per program)
    };
    auto otherProgram = [&]() { iprogGlob = -1; };
    // the shadow phase's DATE mark (FMarkDate): open between ShadowBegin and ShadowCombine; fDateMarked while the
    // stencil holds it, cleared by a pass run that writes the frame's alpha (its stencil writes rewrite the bit)
    bool fShadowPhase = false, fDateMarked = false;
    auto markDate = [&]() {
        if (fDateMarked || !fShadowPhase) return fDateMarked;
        fDateMarked = FMarkDate();
        if (fDateMarked) { otherProgram(); sc.Invalidate(); }
        return fDateMarked;
    };
    static const bool s_fCelAlways = std::getenv("SLY2_CELALWAYS") != nullptr;
#ifdef __SWITCH__
    // (DrawShadow2's plane for its z-pass casters: the drawn near plane (aposNear) moved toward the scene by a margin)
    glm::vec4 planeNearSh(0.0f);
    {
        glm::vec3 n = glm::cross(aposNear[1] - aposNear[0], aposNear[3] - aposNear[0]);
        const float len = glm::length(n);
        n = len > 0.0f ? n / len : glm::normalize(cam.Dir());
        if (glm::dot(n, aposNear[0] - cam.pos) < 0.0f) n = -n;
        const float dNear = glm::dot(n, aposNear[0] - cam.pos);
        planeNearSh = glm::vec4(n, -(glm::dot(n, aposNear[0]) + 1.0f + 0.01f * std::max(dNear, 0.0f)));
    }
    // (s_rectShPhase for the shadow phase opened at icmdBegin: its volumes' screen box, a pixel or two around. The
    // prisms' triangles clipped at half the near plane's distance (the GPU keeps less of them), the hulls by their
    // bounding spheres' boxes; the whole frame when that fails or when multisampled (the copy resolves it whole).)
    auto shadowPhaseRect = [&](size_t icmdBegin) -> std::array<int, 4> {
        const std::array<int, 4> full = {0, 0, pv.dx, pv.dy};
        if (pv.fbo != g_gl.fbo) return full;
        const glm::vec3 nClip(planeNearSh);
        const float dClip = 0.5f * glm::dot(nClip, aposNear[0] - cam.pos);
        if (!(dClip > 0.0f)) return full;
        auto sClip = [&](const glm::vec3& p) { return glm::dot(nClip, p - cam.pos) - dClip; };
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        bool fFull = false;
        auto addPoint = [&](const glm::vec3& p) {
            const glm::vec4 c = matWorldToClip * glm::vec4(p, 1.0f);
            if (!(c.w > 0.0f)) { fFull = true; return; }
            const float x = (c.x / c.w * 0.5f + 0.5f) * float(pv.dx), y = (c.y / c.w * 0.5f + 0.5f) * float(pv.dy);
            x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
        };
        for (size_t i = icmdBegin + 1; i < s_acmd.size() && !fFull && s_acmd[i].cmdk != CMDK_ShadowCombine; ++i)
        {
            const CMD& c = s_acmd[i];
            if (c.cmdk == CMDK_Shadow2)
                for (uint32_t k = 0; k + 2 < c.count && !fFull; k += 3)
                {
                    const glm::vec3* pt = &s_avtxShadow2[c.firstIndex + k];
                    const float as[3] = {sClip(pt[0]), sClip(pt[1]), sClip(pt[2])};
                    for (int e = 0; e < 3; ++e)
                    {
                        const int e1 = (e + 1) % 3;
                        if (as[e] > 0.0f) addPoint(pt[e]);
                        if ((as[e] > 0.0f) != (as[e1] > 0.0f)) addPoint(pt[e] + (pt[e1] - pt[e]) * (as[e] / (as[e] - as[e1])));
                    }
                }
            else if (c.cmdk == CMDK_Shadow)
            {
                const GLOB& geom = *c.prec->pglobGeom;
                if (!geom.pgl || !(geom.pgl->sRadius > 0.0f)) { fFull = true; break; }
                const glm::mat4& m = *c.prec->pmat;
                const float scale = std::max({glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))});
                const glm::vec3 ctr = glm::vec3(m * glm::vec4(geom.pgl->posCenter, 1.0f));
                const float r = geom.pgl->sRadius * scale * 1.05f;
                for (int k = 0; k < 8 && !fFull; ++k) addPoint(ctr + glm::vec3((k & 1) ? r : -r, (k & 2) ? r : -r, (k & 4) ? r : -r));
            }
        }
        if (fFull) return full;
        if (x1 < x0 || y1 < y0) return {0, 0, 0, 0};
        const int ix0 = std::clamp(int(std::floor(x0)) - 2, 0, pv.dx), iy0 = std::clamp(int(std::floor(y0)) - 2, 0, pv.dy);
        const int ix1 = std::clamp(int(std::ceil(x1)) + 2, 0, pv.dx), iy1 = std::clamp(int(std::ceil(y1)) + 2, 0, pv.dy);
        return {ix0, iy0, ix1 - ix0, iy1 - iy0};
    };
#endif
    for (const RUN& run : s_arun)
    {
        const CMD& cmd = s_acmd[run.icmd];
        switch (cmd.cmdk)
        {
        case CMDK_Pass:
        {
            const DSTATE& st = s_astate[cmd.istate];
            const bool fKill = FStateNeedsKill(st);
#ifdef __SWITCH__
            // (the Switch: the blended kinds' runs keep the program with discards, which would otherwise change nearly
            // every run; it draws a run without alpha test the same, its discards needing ate and afailMode, or fDate)
            const bool fKeepKill = cmd.prec && fSortedKind(cmd.prec->kind);
#else
            constexpr bool fKeepKill = false;
#endif
            useGlob(fKill || fKeepKill ? 0 : 1);
            // (bindless: a run with handles leaves the texture unit alone; one without, a single state, binds it)
            sc.Apply(st, !g_fGlobBindless || !HandleOfIstate(cmd.istate));
            if (g_fDirectRuns)
                for (uint32_t i = run.ideiFirst; i < run.ideiFirst + run.cdei; ++i)
                    glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, GLsizei(s_adei[i].count), GL_UNSIGNED_INT,
                                                                  reinterpret_cast<void*>(size_t(s_adei[i].firstIndex) * 4), 1,
                                                                  s_adei[i].baseVertex, s_adei[i].baseInstance);
            else
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                                            reinterpret_cast<void*>(offIndirect + size_t(run.ideiFirst) * sizeof(DEICMD)), GLsizei(run.cdei), 0);
            // (its alpha writes rewrite the stencil's FBA bit: the DATE mark is made again before the next DATE draw)
            if (st.colorMask & 2) fDateMarked = false;
            s_glstats.crun += 1; s_glstats.cpass += run.cdei;
            g_renderstats.cpass += int(run.cdei);
            ++g_renderstats.cbatch;
            break;
        }
        case CMDK_Volume:
            // (no alpha test, no DATE: the program without discards)
            useGlob(1);
            DrawProjVolume(cmd);
            fDateMarked = false;   // (its mask is the stencil's bit 7, cleared after)
            sc.Invalidate();
            ++g_renderstats.cbatch;
            break;
        case CMDK_Shadow:
        {
            // a hull's DATE from the stencil mark when the phase has one: then no discard
            const bool fDate = FHullDate(cmd);
            const bool fMarked = fDate && markDate();
            useGlob(!fDate || fMarked ? 1 : 0);
            DrawShadow(cmd, cam.pos, sNearClipSh, aposNear, fMarked);
            sc.Invalidate();
            ++g_renderstats.cbatch;
            break;
        }
        case CMDK_ShadowBegin:
#ifdef __SWITCH__
            s_rectShPhase = shadowPhaseRect(run.icmd);
#endif
            ShadowBegin(pv.fbo, pv.dx, pv.dy, int(cmd.order));
            fShadowPhase = true;
            fDateMarked = false;
            sc.Invalidate();   // (its stencil clear leaves the write mask at 0xFF)
            ++g_renderstats.cbatch;
            break;
        case CMDK_ShadowKind8:
            ShadowKind8();
            sc.Invalidate();   // (likewise)
            ++g_renderstats.cbatch;
            break;
        case CMDK_FbaClear:
            ClearFbaBits();
            sc.Invalidate();
            break;
        case CMDK_ShadowCombine:
            ShadowCombine(cmd.count & 1, cmd.count & 2);
            fShadowPhase = false;
            fDateMarked = false;
            otherProgram();
            sc.Invalidate();
            ++g_renderstats.cbatch;
            break;
#if !defined(__SWITCH__) && !defined(__APPLE__)
        case CMDK_ShadowMap:
            DrawShadowMaps(matWorldToClip);
            otherProgram();
            sc.Invalidate();
            ++g_renderstats.cbatch;
            break;
#endif
        case CMDK_Shadow2:
        {
            const bool fMarked = !(cmd.order & 8) && markDate();
#ifdef __SWITCH__
            DrawShadow2(cmd, matWorldToClip, fMarked, &planeNearSh);
#else
            DrawShadow2(cmd, matWorldToClip, fMarked);
#endif
            otherProgram();
            sc.Invalidate();
            ++g_renderstats.cbatch;
            break;
        }
        case CMDK_Particles:
            glDisable(GL_STENCIL_TEST);
            if (opts.fMeshes) DrawParticles(matWorldToClip, cam.pos, aspect);
            otherProgram();
            sc.Invalidate();
            ++g_renderstats.cbatch;
            break;
        case CMDK_Cel:
        {
            const GLOB& geom = *cmd.prec->pglobGeom;
            glDisable(GL_CULL_FACE);
            glDisable(GL_STENCIL_TEST);
            glCelBorderShader.Use();
            otherProgram();
            sc.Invalidate();
            glUniformMatrix4fv(s_uc.matWorldToClip, 1, GL_FALSE, glm::value_ptr(matWorldToClip));
            glUniformMatrix4fv(s_uc.matModel, 1, GL_FALSE, glm::value_ptr(*cmd.prec->pmat));
            // Half a PS2 pixel (448 lines) in NDC, square pixels; the line is one PS2 pixel wide (AA1 line + plain).
            const float hy = 1.0f / 448.0f;
            glUniform2f(s_uc.halfWidthNdc, hy / aspect, hy);
            glUniform1i(s_uc.edgeBase, 0);
            glm::vec4 rgba = Vec4FromRgba(opts.rgbaCel, 1.0f / 255.0f);
            // the damage flash (qw8.z): the border toward red (the reference's border at flash 0.5 / 0.75 / 1: red 87 /
            // 123 / 159 of 255, linear in the flash)
            if (const float gFlash = cmd.prec->psgs ? cmd.prec->psgs->gFlash : 0.0f; 0.0f < gFlash)
                rgba = glm::vec4(glm::mix(glm::vec3(rgba), glm::vec3(159.0f / 255.0f, 0.0f, 0.0f), std::min(gFlash, 1.0f)), rgba.a);
            // P_A: the alpha times 0.6 times qw8.y while the object fades (runtime/native_cel_geometry.cpp)
            const float a2 = cmd.prec->alpha2;
            glUniform4f(s_uc.celRgba, rgba.r, rgba.g, rgba.b, std::min(rgba.a * 255.0f / 128.0f, 1.0f) * (a2 < 1.0f ? a2 * 0.6f : 1.0f));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, geom.pgl->ssboEdges);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, geom.pgl->ssboEdgeSkin);
            glUniform1i(s_uc.cbone, cmd.prec->pamatBone ? GLint(cmd.prec->pamatBone->size()) : 0);
            glUniform1i(s_uc.iboneFirst, GLint(cmd.prec->iboneFirst));
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(s_fCelAlways ? GL_ALWAYS : GL_GEQUAL);
            glDepthMask(GL_FALSE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            // (character smoothing) a border with normals drawn as the surfaces' split of each edge, bent as they are
            GLsizei cseg = 1;
            if (g_fSmoothCel)
            {
                cseg = geom.pgl->ssboEdgeNrm ? GLsizei(g_cSmoothGlob) : 1;
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, geom.pgl->ssboEdgeNrm ? geom.pgl->ssboEdgeNrm : geom.pgl->ssboEdges);
                glUniform1i(s_uc.csegSmooth, GLint(cseg));
            }
            glBindVertexArray(s_vaoEmpty);
            glDrawArrays(GL_TRIANGLES, 0, geom.pgl->cedge * 6 * cseg);
            ++g_renderstats.ccel;
            ++g_renderstats.cbatch;
            break;
        }
        }
    }
    if (s_cframeGlStats && pv.irtgt < 0 && !pv.fForeground)
    {
        static int s_cframe = 0;
        if (++s_cframe % s_cframeGlStats == 0)
        {
            const double n = s_cframeGlStats;
            std::printf("glstats: per frame %.0f runs (%.0f draws), %.0f applies (%.0f full), %.0f texture binds, %.0f sampler binds, %.0f uniforms, %.0f blend%s%s\n",
                        s_glstats.crun / n, s_glstats.cpass / n, s_glstats.capply / n, s_glstats.cfull / n, s_glstats.ctex / n, s_glstats.csampler / n,
                        s_glstats.cuniform / n, s_glstats.cblend / n, g_fGlobBindless ? " (bindless)" : "", s_ring.buf ? " (persistent ring)" : "");
            s_glstats = {};
        }
    }
    glBindVertexArray(0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
}
}  // namespace

namespace
{
// orig: 0x001BE990 / 0x001BE9E8 (each owned target: its camera's view / projection (0x1BE6F0), the clear packet)
// then the target's kinds 15..17 / 18..20 drawn into it.
void RenderRenderTargets(const SCENE& scene, const RENDEROPTS& opts)
{
    for (int i = 0; i < CRTGT; ++i)
    {
        RTGT* p = PrtgtGet(i);
        if (!p || !p->powner) continue;
        GlTextureOfRtgt(i);   // (the FBO)
        PASSVIEW pv;
        pv.irtgt = i;
        pv.fbo = p->fbo;
        pv.dx = p->dx;
        pv.dy = p->dy;
        pv.aspect = float(p->dx) / float(p->dy);
        pv.pos = p->pos;
        pv.dir = glm::normalize(p->dirForward);
        // reversed-Z infinite far, the target's aspect (BuildSimpleProjectionMatrix with dx / (tan x aspect), dy / tan)
        const float f = 1.0f / std::tan(p->radFovY * 0.5f);
        glm::mat4 proj(0.0f);
        proj[0][0] = f / pv.aspect;
        proj[1][1] = f;
        proj[2][3] = -1.0f;
        proj[3][2] = p->sNear;
        pv.matWorldToClip = proj * glm::lookAt(p->pos, p->pos + pv.dir, p->dirUp);
        pv.rgbaClear = 0;   // 0x1BE398's clear: RGBAQ 0, Z 0
        pv.alphaClear = 0;
        RenderPass(scene, pv, opts);
    }
}
}  // namespace

void RenderScene(const SCENE& scene, const CAMERA& cam, const RENDEROPTS& opts)
{
    g_renderstats.cdraw = g_renderstats.cpass = g_renderstats.ccel = 0;
    g_renderstats.cbatch = g_renderstats.cculled = g_renderstats.cmrd = 0;
    s_ring.BeginFrame();
    // The render targets first (their globsets, their cameras), so this frame's screen quads show them.
    RenderRenderTargets(scene, opts);
    PASSVIEW pv;
    pv.aspect = float(g_gl.renderWidth) / float(g_gl.renderHeight);
    // The projection for the picture's aspect mode (display.h: Hor+, the PS2 anamorphic 4:3, Vert- 16:9).
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    CAMERA camDisplay = cam;
    camDisplay.fovY = glm::degrees(proj.radFovY);
    pv.matWorldToClip = camDisplay.MatWorldToClip(proj.rAspect);
    pv.rMRDAdjust = cam.rMRDAdjust;
    pv.grfChannels = cam.grfChannels;
    pv.pos = cam.pos;
    pv.dir = cam.Dir();
    pv.fbo = g_gl.FboScene();
    pv.dx = g_gl.renderWidth;
    pv.dy = g_gl.renderHeight;
    pv.rgbaClear = opts.rgbaClear;
    RenderPass(scene, pv, opts);
#if !defined(__SWITCH__) && !defined(__APPLE__)
    // (PC TEST: the level's own shadows over the 3D picture, before the post-processing: display.h worldShadows)
    StepAddedGpuTimers();
    BeginAddedGpuTimer(AGT_Masks);
    s_fTransMaskFrame = g_display.outlines > 0 && FBuildTranslucentMask(scene, pv.matWorldToClip, pv.fbo, pv.dx, pv.dy);
    s_fCharMaskFrame = (NWorldShadowsNow() > 0 || g_display.ambientOcclusion > 0) && FBuildCharacterMask(scene, pv.matWorldToClip, cam.pos, pv.fbo, pv.dx, pv.dy);
    EndAddedGpuTimer(AGT_Masks);
    // (PC TEST: the lamps' cube maps first, display.h lampShadows: where a lamp lights a surface the level's shadow
    // fades; then the lamps' own shadows)
    s_clampFrame = 0;
    if (NWorldShadowsNow() > 0 && NLampShadowsNow() > 0) CBuildLampMaps(scene, cam.pos, pv.dir);
    if (NWorldShadowsNow() > 0) DrawWorldShadows(scene, pv.matWorldToClip, cam.pos, pv.dir, pv.fbo, pv.dx, pv.dy);
    ApplyLampShadows(pv.matWorldToClip, pv.fbo, pv.dx, pv.dy);
    // (PC debug: the casters shown, display.h shadowOverlay)
    if (g_display.shadowOverlay > 0) DrawShadowCasterOverlay(scene, pv.matWorldToClip, cam.pos, pv.dir, pv.fbo, pv.dx, pv.dy);
    else s_wspick = WSPICK{};
    // (PC debug: the texture under the crosshair, display.h texturePicker)
    if (g_display.texturePicker > 0) DrawTexturePicker(scene, pv.matWorldToClip, pv.fbo, pv.dx, pv.dy);
    else s_texpick = TEXPICK{};
#endif
}

void RenderSceneForeground(const SCENE& scene, const CAMERA& cam, const RENDEROPTS& opts)
{
    if (std::none_of(scene.aglobset.begin(), scene.aglobset.end(), [](const SCENEGLOBSET& s) { return s.fForeground && s.irtgt < 0; })) return;
    PASSVIEW pv;
    pv.aspect = float(g_gl.renderWidth) / float(g_gl.renderHeight);
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    CAMERA camDisplay = cam;
    camDisplay.fovY = glm::degrees(proj.radFovY);
    pv.matWorldToClip = camDisplay.MatWorldToClip(proj.rAspect);
    pv.rMRDAdjust = cam.rMRDAdjust;
    pv.grfChannels = cam.grfChannels;
    pv.pos = cam.pos;
    pv.dir = cam.Dir();
    pv.fbo = g_gl.FboScene();
    pv.dx = g_gl.renderWidth;
    pv.dy = g_gl.renderHeight;
    pv.fForeground = true;
    RenderPass(scene, pv, opts);
}

// ------------------------------------------------------------ PC: shadow afterimages (render.h RenderGhostPoses)
void RenderGhostPoses(const SCENE& scene, const CAMERA& cam, const std::vector<GHOSTDRAW>& adraw)
{
#if defined(__SWITCH__) || defined(__APPLE__)
    (void)scene; (void)cam; (void)adraw;
#else
    if (adraw.empty() || !g_gl.window) return;
    static GLSHADER s_sh;
    static int s_state = 0;   // 0 not built yet, 1 built, -1 failed (no afterimages this session)
    static GLint s_locClip = -1, s_locModel = -1, s_locBoneFirst = -1, s_locCBone = -1, s_locTex = -1, s_locTextured = -1, s_locRgba = -1, s_locMode = -1;
    static GLuint s_ssbo = 0;
    if (s_state == 0)
    {
        s_state = s_sh.FInit("ghost.vert", "ghost.frag", "") ? 1 : -1;
        if (s_state < 0) { std::printf("afterimages: ghost.vert / ghost.frag did not build: none drawn\n"); return; }
        s_locClip = s_sh.Loc("matWorldToClip");
        s_locModel = s_sh.Loc("matModel");
        s_locBoneFirst = s_sh.Loc("iboneFirst");
        s_locCBone = s_sh.Loc("cbone");
        s_locTex = s_sh.Loc("tex");
        s_locTextured = s_sh.Loc("fTextured");
        s_locRgba = s_sh.Loc("rgba");
        s_locMode = s_sh.Loc("mode");
        glGenBuffers(1, &s_ssbo);
    }
    if (s_state < 0) return;
    InitCommonOnce();   // (the samplers)

    // One draw per strip group's entry, with its pose's bones in the buffer's order.
    struct GDRAW
    {
        GLuint vao;
        int32_t baseVertex;
        uint32_t firstIndex, count;
        glm::mat4 mat;
        uint32_t iboneFirst, cbone;
        GLuint tex;
        uint8_t sampler;
        bool fTextured;
    };
    static std::vector<glm::mat4> s_amatBone;
    static std::vector<GDRAW> s_agd;
    static std::vector<size_t> s_aigdEnd;   // per pose, its draws' end in s_agd
    s_amatBone.clear();
    s_agd.clear();
    s_aigdEnd.clear();
    const SCENEGLOBSET* psgsFirst = scene.aglobset.data();
    const SCENEGLOBSET* psgsEnd = psgsFirst + scene.aglobset.size();
    for (const GHOSTDRAW& gd : adraw)
    {
        if (gd.papart)
            for (const GHOSTPART& part : *gd.papart)
            {
                if (!part.pglobset) continue;
                // (the entry's own vertex copies only while it is still this globset's entry in the scene)
                const SCENEGLOBSET* psgs = part.psgs >= psgsFirst && part.psgs < psgsEnd && part.psgs->pglobset == part.pglobset ? part.psgs : nullptr;
                const GLOBSET& gs = *part.pglobset;
                for (size_t i = 0; i < gs.aglob.size(); ++i)
                {
                    if (i >= part.afVisible.size() || !part.afVisible[i] || i >= part.amatGlob.size()) continue;
                    const GLOB& glob = gs.aglob[i];
                    const GLOB* pgeom = &glob;
                    if (!glob.pgl && glob.iglobTemplate >= 0 && size_t(glob.iglobTemplate) < gs.aglob.size()) pgeom = &gs.aglob[size_t(glob.iglobTemplate)];
                    if (pgeom->meshk != MESHK_Strips || !pgeom->pgl) continue;   // (cindex is set for the shadow meshes only)
                    const GLGLOB* pgl = psgs && i < psgs->apglInst.size() && psgs->apglInst[i] && psgs->apglInst[i]->vao ? psgs->apglInst[i] : pgeom->pgl;
                    uint32_t iboneFirst = 0, cbone = 0;
                    if (i < part.aamatBone.size() && !part.aamatBone[i].empty())
                    {
                        iboneFirst = uint32_t(s_amatBone.size());
                        cbone = uint32_t(part.aamatBone[i].size());
                        s_amatBone.insert(s_amatBone.end(), part.aamatBone[i].begin(), part.aamatBone[i].end());
                    }
                    for (const SUBGLOB& sub : pgeom->asubglob)
                    {
                        if (!sub.pshd || sub.indexCount == 0 || sub.pshd->shdk == 3) continue;   // (no projected volumes)
                        if (gd.fAdditiveOnly)
                        {
                            // (the first pass blended (Cs - 0) x As + Cd: GS ALPHA's B 2 (zero), D 1 (the frame))
                            if (sub.pshd->apass.empty()) continue;
                            const SHDPASS& ps = sub.pshd->apass[0];
                            if (!ps.fBlend || ((ps.alpha >> 2) & 3) != 2 || ((ps.alpha >> 6) & 3) != 1) continue;
                        }
                        GDRAW d{pgl->vao, pgl->baseVertex, pgl->firstIndex + sub.firstIndex, sub.indexCount, part.amatGlob[i], iboneFirst, cbone, 0, 0, false};
                        // the texture of its first pass, as the scene drew it (for the cut-outs)
                        if (const SUBGLOBGPU* pgpu = sub.pgpu; pgpu && !pgpu->apasscmd.empty() && pgpu->apasscmd[0].istate < s_astate.size())
                        {
                            const DSTATE& st = s_astate[pgpu->apasscmd[0].istate];
                            if (st.fTextured && st.tex) { d.tex = st.tex; d.sampler = st.sampler; d.fTextured = true; }
                        }
                        s_agd.push_back(d);
                    }
                }
            }
        s_aigdEnd.push_back(s_agd.size());
    }
    if (s_agd.empty()) return;

    // GL state: saved, then the passes' own
    GLint prog = 0, vao = 0, fboDraw = 0, activeTex = 0, tex0 = 0, sampler0 = 0, depthFunc = 0, aviewport[4] = {};
    GLint stencilFunc = 0, stencilRef = 0, stencilValueMask = 0, stencilWriteMask = 0, sfail = 0, dpfail = 0, dppass = 0, clearStencil = 0;
    GLint blendSrcRgb = 0, blendDstRgb = 0, blendSrcA = 0, blendDstA = 0, blendEqRgb = 0, blendEqA = 0;
    GLboolean fBlend = glIsEnabled(GL_BLEND), fDepth = glIsEnabled(GL_DEPTH_TEST), fStencil = glIsEnabled(GL_STENCIL_TEST),
              fCull = glIsEnabled(GL_CULL_FACE), fScissor = glIsEnabled(GL_SCISSOR_TEST), fDepthMask = GL_TRUE;
    GLboolean acolorMask[4] = {};
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboDraw);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler0);
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    glGetIntegerv(GL_VIEWPORT, aviewport);
    glGetIntegerv(GL_STENCIL_FUNC, &stencilFunc);
    glGetIntegerv(GL_STENCIL_REF, &stencilRef);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &stencilValueMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilWriteMask);
    glGetIntegerv(GL_STENCIL_FAIL, &sfail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &dpfail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &dppass);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &clearStencil);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqA);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &fDepthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, acolorMask);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl.FboScene());
    glViewport(0, 0, g_gl.renderWidth, g_gl.renderHeight);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);   // (reversed Z: nearer is greater; behind the scene's surfaces, Sly's own included)
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);   // (the frame's alpha is the game's: FBA, DATE)
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_NOTEQUAL, 1, 1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glClearStencil(0);
    auto clearBit0 = [] {
        glStencilMask(1);
        glClear(GL_STENCIL_BUFFER_BIT);
    };

    s_sh.Use();
    glBindTexture(GL_TEXTURE_2D, 0);   // (until a textured entry binds its own: never the target's own texture)
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    CAMERA camDisplay = cam;
    camDisplay.fovY = glm::degrees(proj.radFovY);
    const glm::mat4 matWorldToClip = camDisplay.MatWorldToClip(proj.rAspect);
    glUniformMatrix4fv(s_locClip, 1, GL_FALSE, glm::value_ptr(matWorldToClip));
    glUniform1i(s_locTex, 0);
    glUniform3f(s_sh.Loc("posEye"), cam.pos.x, cam.pos.y, cam.pos.z);
    const GLint locTowardEye = s_sh.Loc("sTowardEye");
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(std::max<size_t>(1, s_amatBone.size()) * sizeof(glm::mat4)),
                 s_amatBone.empty() ? nullptr : s_amatBone.data(), GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, s_ssbo);
    size_t igd = 0;
    for (size_t ipose = 0; ipose < adraw.size(); ++ipose)
    {
        const size_t igdEnd = s_aigdEnd[ipose];
        if (igd == igdEnd) continue;
        const bool fGlow = adraw[ipose].mode == 1;
        glUniform1i(s_locMode, fGlow ? 1 : 0);
        if (fGlow)
        {
            // (a glow: added, every layer of it, in front of the scene's surfaces only)
            glDisable(GL_STENCIL_TEST);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
        }
        else
        {
            glEnable(GL_STENCIL_TEST);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            clearBit0();
            glStencilMask(1);
        }
        // (over its own surfaces: equal depths pass, pulled a little nearer so the two skinnings never fight)
        // (and toward the camera: a smoothed character, glob.geom's PN triangles, bulges up to a few units out of the
        // triangles the silhouette is drawn from, where it was hidden in patches; a little, unsmoothed)
        if (adraw[ipose].fOver)
        {
            glDepthFunc(GL_GEQUAL);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 64.0f);   // (reversed Z: a greater depth is nearer)
            glUniform1f(locTowardEye, g_cSmoothGlob > 0 ? 6.0f : 1.5f);
        }
        else
        {
            glDepthFunc(GL_GREATER);
            glDisable(GL_POLYGON_OFFSET_FILL);
            glUniform1f(locTowardEye, 0.0f);
        }
        const glm::vec4& c = adraw[ipose].rgba;
        glUniform4f(s_locRgba, c.r, c.g, c.b, c.a);
        GLuint vaoCur = 0, texCur = 0;
        int fTexturedCur = -1;
        for (; igd < igdEnd; ++igd)
        {
            const GDRAW& d = s_agd[igd];
            if (d.vao != vaoCur) { glBindVertexArray(d.vao); vaoCur = d.vao; }
            if (int(d.fTextured) != fTexturedCur) { glUniform1i(s_locTextured, d.fTextured ? 1 : 0); fTexturedCur = int(d.fTextured); }
            if (d.fTextured && d.tex != texCur)
            {
                glBindTexture(GL_TEXTURE_2D, d.tex);
                glBindSampler(0, s_asampler[d.sampler & 15u]);
                texCur = d.tex;
            }
            glUniform1i(s_locCBone, GLint(d.cbone));
            glUniform1i(s_locBoneFirst, GLint(d.iboneFirst));
            if (!d.cbone) glUniformMatrix4fv(s_locModel, 1, GL_FALSE, glm::value_ptr(d.mat));
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(d.count), GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(d.firstIndex) * 4), d.baseVertex);
        }
    }
    clearBit0();

    glUseProgram(GLuint(prog));
    glBindVertexArray(GLuint(vao));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fboDraw));
    glBindTexture(GL_TEXTURE_2D, GLuint(tex0));
    glBindSampler(0, GLuint(sampler0));
    glActiveTexture(GLenum(activeTex));
    glViewport(aviewport[0], aviewport[1], aviewport[2], aviewport[3]);
    glDisable(GL_POLYGON_OFFSET_FILL);   // (fOver: the renderer keeps it off)
    glDepthFunc(GLenum(depthFunc));
    glDepthMask(fDepthMask);
    glColorMask(acolorMask[0], acolorMask[1], acolorMask[2], acolorMask[3]);
    glBlendEquationSeparate(GLenum(blendEqRgb), GLenum(blendEqA));
    glBlendFuncSeparate(GLenum(blendSrcRgb), GLenum(blendDstRgb), GLenum(blendSrcA), GLenum(blendDstA));
    glStencilFunc(GLenum(stencilFunc), stencilRef, GLuint(stencilValueMask));
    glStencilOp(GLenum(sfail), GLenum(dpfail), GLenum(dppass));
    glStencilMask(GLuint(stencilWriteMask));
    glClearStencil(clearStencil);
    auto set = [](GLenum cap, GLboolean f) { if (f) glEnable(cap); else glDisable(cap); };
    set(GL_BLEND, fBlend);
    set(GL_DEPTH_TEST, fDepth);
    set(GL_STENCIL_TEST, fStencil);
    set(GL_CULL_FACE, fCull);
    set(GL_SCISSOR_TEST, fScissor);
#endif
}

// ------------------------------------------------------------ PC: glowing edges through walls (render.h RenderXrayOutlines)
void RenderXrayOutlines(const SCENE& scene, const CAMERA& cam, const std::vector<XRAYPART>& apart, const glm::vec3& rgb)
{
#if defined(__SWITCH__) || defined(__APPLE__)
    (void)scene; (void)cam; (void)apart; (void)rgb;
#else
    (void)scene;
    if (apart.empty() || !g_gl.window) return;
    static GLSHADER s_shMask, s_shGlow;
    static int s_state = 0;
    static GLuint s_tex = 0, s_fbo = 0;
    static int s_dx = 0, s_dy = 0;
    if (s_state == 0)
    {
        s_state = s_shMask.FInit("smdepth.vert", "wsdebug.frag", "") && s_shGlow.FInit("shadowcombine.vert", "xrayglow.frag", "") ? 1 : -1;
        std::printf("glowing edges: %s\n", s_state > 0 ? "built" : "did not build: none drawn");
    }
    if (s_state < 0) return;
    const int dx = g_gl.renderWidth, dy = g_gl.renderHeight;
    if (!s_fbo || dx != s_dx || dy != s_dy)
    {
        if (s_fbo) glDeleteFramebuffers(1, &s_fbo);
        if (s_tex) glDeleteTextures(1, &s_tex);
        GLint texPrev = 0, fboPrev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texPrev);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboPrev);
        glGenTextures(1, &s_tex);
        glBindTexture(GL_TEXTURE_2D, s_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, dx, dy);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, GLuint(texPrev));
        glGenFramebuffers(1, &s_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex, 0);
        const GLenum buf = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &buf);
        const bool fOk = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fboPrev));
        if (!fOk) { std::printf("glowing edges: the mask's framebuffer is not usable here\n"); s_state = -1; return; }
        s_dx = dx, s_dy = dy;
    }
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    CAMERA camDisplay = cam;
    camDisplay.fovY = glm::degrees(proj.radFovY);
    const glm::mat4 matWorldToClip = camDisplay.MatWorldToClip(proj.rAspect);
    const float radius = std::max(3.0f, 7.0f * float(dy) / 1080.0f);

    // the draws, and the screen's rectangle they and their glow cover (the glow pass only there)
    struct XDRAW { GLuint vao; int32_t baseVertex; uint32_t firstIndex, count; const glm::mat4* pmat; uint32_t iboneFirst, cbone; float g; };
    static std::vector<XDRAW> s_ad;
    s_ad.clear();
    float x0 = float(dx), y0 = float(dy), x1 = 0.0f, y1 = 0.0f;
    for (const XRAYPART& part : apart)
    {
        const SCENEGLOBSET* psgs = part.psgs;
        if (!psgs || !psgs->pglobset || part.iglob >= psgs->pglobset->aglob.size()) continue;
        const GLOB* pgeom = PglobGeometry(*psgs->pglobset, part.iglob);
        if (!pgeom) continue;
        bool fAny = false;
        for (const SUBGLOB& sub : pgeom->asubglob)
        {
            const WSWHY why = WsWhySub(sub);
            if (why == WSWHY_Empty || why == WSWHY_LightVolume || why == WSWHY_Ps2Shadow) continue;   // (its body; its glow too)
            s_ad.push_back({pgeom->pgl->vao, pgeom->pgl->baseVertex, pgeom->pgl->firstIndex + sub.firstIndex, sub.indexCount, &part.mat, 0, 0, part.g});
            fAny = true;
        }
        if (!fAny) continue;
        // its sphere on the screen (the whole screen when it reaches behind the camera)
        const glm::vec4 sphere = SphereOfGlob(*pgeom);
        const float rScale = std::max({glm::length(glm::vec3(part.mat[0])), glm::length(glm::vec3(part.mat[1])), glm::length(glm::vec3(part.mat[2]))});
        const glm::vec3 c = glm::vec3(part.mat * glm::vec4(sphere.w >= 0.0f ? glm::vec3(sphere) : glm::vec3(0.0f), 1.0f));
        const float r = sphere.w >= 0.0f ? sphere.w * rScale : 300.0f;
        const glm::vec4 pc = matWorldToClip * glm::vec4(c, 1.0f);
        if (pc.w <= r + 1.0f) { x0 = y0 = 0.0f; x1 = float(dx); y1 = float(dy); continue; }
        const glm::vec2 ps = (glm::vec2(pc) / pc.w * 0.5f + 0.5f) * glm::vec2(float(dx), float(dy));
        const float rpx = r / pc.w * std::max(std::fabs(matWorldToClip[0][0]) * float(dx), std::fabs(matWorldToClip[1][1]) * float(dy)) * 0.5f * 1.2f + radius + 2.0f;
        x0 = std::min(x0, ps.x - rpx), y0 = std::min(y0, ps.y - rpx), x1 = std::max(x1, ps.x + rpx), y1 = std::max(y1, ps.y + rpx);
    }
    x0 = std::max(x0, 0.0f), y0 = std::max(y0, 0.0f), x1 = std::min(x1, float(dx)), y1 = std::min(y1, float(dy));
    if (s_ad.empty() || x1 <= x0 || y1 <= y0) return;
    const GLint sx = GLint(x0), sy = GLint(y0), sw = GLint(std::ceil(x1)) - sx, sh = GLint(std::ceil(y1)) - sy;

    SMSAVE save;
    GLint texUnit0 = 0, samplerUnit0 = 0, aboxScissor[4] = {};
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texUnit0);
    glGetIntegerv(GL_SAMPLER_BINDING, &samplerUnit0);
    glGetIntegerv(GL_SCISSOR_BOX, aboxScissor);
    // the mask: every draw where it is on the screen, walls or not (the brightest kept)
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, dx, dy);
    glEnable(GL_SCISSOR_TEST);
    glScissor(sx, sy, sw, sh);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_BLEND);
    glBlendEquation(GL_MAX);
    glBlendFunc(GL_ONE, GL_ONE);
    s_shMask.Use();
    glUniformMatrix4fv(s_shMask.Loc("matWorldToLight"), 1, GL_FALSE, glm::value_ptr(matWorldToClip));
    const GLint locModel = s_shMask.Loc("matModel"), locCBone = s_shMask.Loc("cbone"), locBoneFirst = s_shMask.Loc("iboneFirst"), locRgba = s_shMask.Loc("rgba");
    GLuint vaoCur = 0;
    for (const XDRAW& d : s_ad)
    {
        if (d.vao != vaoCur) glBindVertexArray(vaoCur = d.vao);
        const float g = std::clamp(d.g, 1.0f / 255.0f, 1.0f);
        glUniform4f(locRgba, g, g, g, g);
        glUniform1i(locCBone, GLint(d.cbone));
        glUniform1i(locBoneFirst, GLint(d.iboneFirst));
        glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(*d.pmat));
        glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(d.count), GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(d.firstIndex) * 4), d.baseVertex);
    }
    // the glow, added over the frame (the alpha kept: the game's)
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl.FboScene());
    glViewport(0, 0, dx, dy);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    s_shGlow.Use();
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glBindSampler(0, 0);
    glUniform1i(s_shGlow.Loc("texMask"), 0);
    glUniform3f(s_shGlow.Loc("rgb"), rgb.r, rgb.g, rgb.b);
    glUniform1f(s_shGlow.Loc("radius"), radius);
    glBindVertexArray(s_vaoEmpty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, GLuint(texUnit0));
    glBindSampler(0, GLuint(samplerUnit0));
    glScissor(aboxScissor[0], aboxScissor[1], aboxScissor[2], aboxScissor[3]);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
#endif
}

// (PC debug) the group under the crosshair of the casters' overlay, as of the last frame
const WSPICK& WspickCur()
{
#if !defined(__SWITCH__) && !defined(__APPLE__)
    return s_wspick;
#else
    static const WSPICK s_none;
    return s_none;
#endif
}

const TEXPICK& TexpickCur()
{
#if !defined(__SWITCH__) && !defined(__APPLE__)
    return s_texpick;
#else
    static const TEXPICK s_none;
    return s_none;
#endif
}

bool FCopyPickedTextureToMods(std::string* pstr)
{
#if !defined(__SWITCH__) && !defined(__APPLE__)
    if (!s_texpick.fValid || !s_texpick.fTextured || !s_pbmpPick) { *pstr = "no texture under the crosshair"; return false; }
    std::vector<uint8_t> rgba;
    std::string err;
    if (!FDecodeTexture(s_pbmpPick, s_pclutPick, rgba, err)) { *pstr = "the texture did not decode: " + err; return false; }
    std::string path;
    bool fExisted = false;
    if (!FCopyTextureToMods(rgba, s_pbmpPick->dx, s_pbmpPick->dy, &path, &fExisted)) { *pstr = "not written (no mods folder?)"; return false; }
    std::printf("texture picker: %s %s\n", fExisted ? "already in the mods folder:" : "copied to", path.c_str());
    *pstr = (fExisted ? "already there: " : "copied: ") + s_texpick.name;
    return true;
#else
    *pstr = "not on this platform";
    return false;
#endif
}

bool FExportPickedModel(const std::string& name, std::string* pstr)
{
#if !defined(__SWITCH__) && !defined(__APPLE__)
    if (!s_texpick.fValid || !s_mxPick.pglobset) { *pstr = "nothing under the crosshair"; return false; }
    if (g_disc.DirMods().empty()) { *pstr = "no mods folder"; return false; }
    std::string stem = name;
    for (char& ch : stem)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') ch = '_';
    char ach[64];
    std::snprintf(ach, sizeof ach, "_oid%d.glb", s_mxPick.oid);
    const std::string path = cane::utf8(cane::path(g_disc.DirMods()) / "Models" / "Export" / cane::path(stem + ach));
    std::string info, err;
    if (!FExportModelGlb(s_mxPick, stem, path, &info, &err)) { *pstr = "not written: " + err; return false; }
    std::printf("model export: %s (%s)\n", path.c_str(), info.c_str());
    *pstr = "Models/Export/" + stem + ach + ": " + info;
    return true;
#else
    (void)name;
    *pstr = "not on this platform";
    return false;
#endif
}

void ReloadTextureMods()
{
    RescanTextureMods();
    ResetTextureCache();
}

const char* PchzWsWhy(int why)
{
    switch (why)
    {
    case WSWHY_Casts: return "casts";
    case WSWHY_Character: return "a character (its own shadows)";
    case WSWHY_Faded: return "faded out";
    case WSWHY_Sky: return "sky-wide";
    case WSWHY_Out: return "out of the shadow box";
    case WSWHY_NotMesh: return "not a mesh";
    case WSWHY_Empty: return "no shader";
    case WSWHY_LightVolume: return "a light's volume";
    case WSWHY_Ps2Shadow: return "the PS2's own shadow shape";
    case WSWHY_Glow: return "an additive glow";
    default: return "?";
    }
}

bool g_fAddedGpuTimes = false;

std::string StrAddedGpuTimes()
{
#if !defined(__SWITCH__) && !defined(__APPLE__)
    if (!g_fAddedGpuTimes) return std::string();
    static const char* const s_apchz[AGT_Max] = {"level map", "level", "lamp maps", "lamps", "masks"};
    std::string str;
    char ach[48];
    double msAll = 0.0;
    for (int k = 0; k < AGT_Max; ++k)
    {
        if (s_aagt[k].ms < 0.005) continue;
        std::snprintf(ach, sizeof ach, "%s%s %.2f", str.empty() ? "" : "   ", s_apchz[k], s_aagt[k].ms);
        str += ach;
        msAll += s_aagt[k].ms;
    }
    if (str.empty()) return str;
    std::snprintf(ach, sizeof ach, "added GPU ms %.2f:   ", msAll);
    return ach + str;
#else
    return std::string();
#endif
}

// (PC) the see-through surfaces' cover of this frame for post.cpp's outlines (0: none)
unsigned TexTranslucentMaskFrame()
{
#if !defined(__SWITCH__) && !defined(__APPLE__)
    return s_fTransMaskFrame ? s_texTransMask : 0u;
#else
    return 0u;
#endif
}

// (PC) the characters' mask of this frame for post.cpp's ambient occlusion (0: none)
unsigned TexCharacterMaskFrame()
{
#if !defined(__SWITCH__) && !defined(__APPLE__)
    return s_fCharMaskFrame ? s_texCharMask : 0u;
#else
    return 0u;
#endif
}
