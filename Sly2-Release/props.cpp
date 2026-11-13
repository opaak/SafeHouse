// PC modding: props (props.h).

#include "props.h"
#include "disc.h"
#include "glob.h"
#include "modelimport.h"
#include "render.h"
#include "savedata.h"
#include "scene.h"
#include "shd.h"
#include "transition.h"
#include "raccoonus.h"
#include "region.h"
#include "sound.h"
#include "../platform/platform.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include "frontend.h"
#include "joy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
int NRef(const CRef& r) { return r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_F32 ? int(r.m_g) : r.m_tagk == TAGK_Bool ? int(r.m_bool) : 0; }
const char* const s_aszEpisode[9] = {"intro", "nightclub", "palace", "temple", "prison", "castle", "train", "sawmill", "blimp"};

// a model: its triangles (the game's space, its origin where it stands), its parts' shaders made once
struct PROPMODEL
{
    std::string file;
    bool fOk = false;
    std::vector<GLTFPRIM> aprim;
    glm::vec4 sphere{0.0f};
    bool fBlend = false;
    std::vector<std::unique_ptr<BMP>> apbmp;
    std::vector<std::unique_ptr<SHD>> apshd;
    std::vector<SHD*> apshdPrim;   // per primitive its shader
};
struct PROP
{
    std::string file;
    glm::vec3 pos{0.0f};
    float turn = 0.0f, pitch = 0.0f, roll = 0.0f, scale = 1.0f;
    int after = 0;   // 0 always; else shown once that episode is complete
    bool fLocked = false;   // (the placement tool leaves it as it is)
    bool fBoss = false;     // (a boss item: looked at closely at the trophy stop)
    std::string title;      // (its name there; "" the file's)
    // (looked at closely: how far it turns from its front, about the up axis and tipped; -1 freely: a model
    // missing its back or underside keeps them out of sight)
    float yawLimit = -1.0f, pitchLimit = -1.0f;
    // made for the GPU at its place (its light is the place's): rebuilt when moved
    SCENEGLOBSET sgs;
    bool fBuilt = false;
};

int s_iwrecProps = -2;
std::vector<std::unique_ptr<PROP>> s_aprop;
std::map<std::string, std::unique_ptr<PROPMODEL>> s_mpmodel;
// (models read again in the same world: the old ones kept until the world changes, the renderer's caches key on their
// textures and shaders)
std::vector<std::unique_ptr<PROPMODEL>> s_apmodelOld;
int s_ipropSel = -1;
std::vector<std::string> s_afileModel;
bool s_fModelsScanned = false;
uint32_t s_cframe = 0;
const SCENE* s_pscene = nullptr;
// the world's views (props.h)
struct PROPVIEW
{
    glm::vec3 pos{0.0f};
    float yaw = 0.0f, pitch = 0.0f, fov = 0.0f;   // (degrees; fov 0: the game camera's)
};
std::map<std::string, PROPVIEW> s_mpview;
CAMERA s_camLast;
bool s_fCamLast = false;
// a boss item looked at closely (props.h)
struct INSPECT
{
    bool fOpen = false, fClosing = false;
    int iprop = -1;
    float u = 0.0f;               // (0 at its place .. 1 before the camera)
    glm::quat q0{1, 0, 0, 0};     // its turn as placed, in the camera's frame (forward, left, up)
    glm::quat q{1, 0, 0, 0};      // as turned
    float zoom = 1.0f, tIdle = 0.0f, t = 0.0f;
    float yaw = 0.0f, pitch = 0.0f, tSway = 0.0f;   // (degrees from q0: about the camera's up, then its left)
    double tPrev = 0.0;
};
INSPECT s_insp;
glm::mat4 MatInspect(const PROP& p, const glm::vec4& sphere);   // (the frame's scene, CollectPropDraws: the surfaces the tool snaps to)

fs::path DirProps() { return cane::path(g_disc.DirMods()) / "Props"; }
fs::path PathWorldProps() { return DirProps() / ("w" + std::to_string(g_transition.iwrecCur) + ".txt"); }

void LoadWorldProps()
{
    s_insp = INSPECT{};
    s_aprop.clear();
    s_mpview.clear();
    s_mpmodel.clear();
    s_apmodelOld.clear();
    s_ipropSel = -1;
    s_iwrecProps = g_transition.iwrecCur;
    if (g_disc.DirMods().empty()) return;
    std::ifstream in(PathWorldProps());
    std::string line;
    int iline = 0;
    while (std::getline(in, line))
    {
        ++iline;
        const size_t ihash = line.find('#');
        if (ihash != std::string::npos) line.resize(ihash);
        std::istringstream is(line);
        auto prop = std::make_unique<PROP>();
        if (!(is >> prop->file)) continue;
        if (prop->file == "view")
        {
            std::string name;
            PROPVIEW v;
            if (is >> name >> v.pos.x >> v.pos.y >> v.pos.z >> v.yaw >> v.pitch)
            {
                if (!(is >> v.fov)) v.fov = 0.0f;
                s_mpview[name] = v;
            }
            else std::printf("props: %s line %d: a view needs <name> <x> <y> <z> <yaw> <pitch>\n", cane::utf8(PathWorldProps()).c_str(), iline);
            continue;
        }
        if (!(is >> prop->pos.x >> prop->pos.y >> prop->pos.z >> prop->turn)) continue;
        std::string key;
        while (is >> key)
        {
            if (key == "scale") is >> prop->scale;
            else if (key == "tilt") is >> prop->pitch >> prop->roll;
            else if (key == "after") is >> prop->after;
            else if (key == "locked") prop->fLocked = true;
            else if (key == "boss") prop->fBoss = true;
            else if (key == "title") is >> prop->title;
            else if (key == "turnlimit") is >> prop->yawLimit >> prop->pitchLimit;
            else std::printf("props: %s line %d: '%s' not known\n", cane::utf8(PathWorldProps()).c_str(), iline, key.c_str());
        }
        prop->scale = std::clamp(prop->scale, 0.01f, 100.0f);
        prop->after = std::clamp(prop->after, 0, 8);
        s_aprop.push_back(std::move(prop));
    }
    if (!s_aprop.empty()) std::printf("props: world %d: %zu props\n", g_transition.iwrecCur, s_aprop.size());
}

// a picture as a texture of the game's (32-bit texels) with a shader drawing it: opaque, cut out, or blended
SHD* PshdMake(PROPMODEL& m, const std::shared_ptr<GLTFIMAGE>& pimg, int alphaMode)
{
    auto pbmp = std::make_unique<BMP>();
    if (pimg && pimg->pab)
    {
        pbmp->dx = uint16_t(pimg->dx), pbmp->dy = uint16_t(pimg->dy);
        pbmp->pab = pimg->pab;
    }
    else
    {
        pbmp->dx = pbmp->dy = 1;
        pbmp->pab = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{255, 255, 255, 128});
    }
    pbmp->psm = 0;
    pbmp->cb = uint32_t(pbmp->pab->size());
    auto pshd = std::make_unique<SHD>();
    pshd->shdk = 0;
    pshd->argba[0] = pshd->argba[1] = pshd->argba[2] = 0x80808080u;
    TEX tex;
    tex.grftex = uint16_t((pimg && pimg->fClampS ? 1 : 0) | (pimg && pimg->fClampT ? 2 : 0));
    tex.pshd = pshd.get();
    tex.apbmp.push_back(pbmp.get());
    pshd->atex.push_back(tex);
    pshd->cframe = 1;
    SHDPASS ps;
    ps.fTextured = true;
    ps.icolor = 0;
    ps.test = (1ull << 16) | (2ull << 17);   // (Z test GEQUAL)
    if (alphaMode == 1) ps.test |= 1ull | (5ull << 1) | (0x40ull << 4), ps.fBlend = false;   // (alpha test GEQUAL 0x40: cut out)
    else if (alphaMode == 2)
    {
        // ((Cs - Cd) As + Cd; the opaque texels write the depth, the rest only the colour: as the game's glass)
        ps.fBlend = true;
        ps.alpha = 0x44;
        ps.test |= 1ull | (5ull << 1) | (0x7Eull << 4) | (1ull << 12);
    }
    else ps.fBlend = false;
    pshd->apass.push_back(ps);
    pshd->cregs = 1;
    SHD* p = pshd.get();
    m.apbmp.push_back(std::move(pbmp));
    m.apshd.push_back(std::move(pshd));
    return p;
}

PROPMODEL* PmodelOf(const std::string& file)
{
    auto& pm = s_mpmodel[file];
    if (pm) return pm->fOk ? pm.get() : nullptr;
    pm = std::make_unique<PROPMODEL>();
    pm->file = file;
    std::string err;
    bool fGame = false;
    if (!FReadGltfPrims(cane::utf8(DirProps() / "Models" / cane::path(file)), pm->aprim, &fGame, err))
    {
        std::printf("props: model %s: %s\n", file.c_str(), err.c_str());
        return nullptr;
    }
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const GLTFPRIM& p : pm->aprim)
        for (const glm::vec3& v : p.apos) lo = glm::min(lo, v), hi = glm::max(hi, v);
    if (fGame)
    {
        // (exported from the game: its middle over the origin, its base on it)
        const glm::vec3 d(-(lo.x + hi.x) * 0.5f, -(lo.y + hi.y) * 0.5f, -lo.z);
        for (GLTFPRIM& p : pm->aprim)
            for (glm::vec3& v : p.apos) v += d;
        lo += d, hi += d;
    }
    pm->sphere = glm::vec4((lo + hi) * 0.5f, glm::length(hi - lo) * 0.5f);
    std::map<std::pair<const GLTFIMAGE*, int>, SHD*> mpshd;
    for (const GLTFPRIM& p : pm->aprim)
    {
        SHD*& pshd = mpshd[{p.pimg.get(), p.alphaMode}];
        if (!pshd) pshd = PshdMake(*pm, p.pimg, p.alphaMode);
        pm->apshdPrim.push_back(pshd);
        pm->fBlend = pm->fBlend || p.alphaMode == 2;
    }
    pm->fOk = true;
    return pm.get();
}

// The level's light at pos: the colours of the level's vertices within reach, nearer weighing more (none: neutral)
glm::vec3 RgbLevelLightAt(const SCENE& scene, const glm::vec3& pos)
{
    const float sReach = 400.0f;
    glm::vec3 sum(0.0f);
    float wSum = 0.0f;
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.pglobset || sgs.irtgt >= 0 || sgs.fForeground || sgs.fActorTree) continue;
        if (sgs.fBounds && sgs.sBoundsRadius < 40000.0f && glm::length(sgs.posBounds - pos) > sgs.sBoundsRadius + sReach) continue;
        const GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size() && i < sgs.amatGlob.size(); ++i)
        {
            const GLOB* pg = &gs.aglob[i];
            if (pg->avtx.empty() && pg->iglobTemplate >= 0 && size_t(pg->iglobTemplate) < gs.aglob.size()) pg = &gs.aglob[size_t(pg->iglobTemplate)];
            if (pg->meshk != MESHK_Strips || pg->avtx.empty() || !pg->abone.empty()) continue;
            const int kind = pg->rp & 0x1F;
            if (kind == 4 || kind == 6) continue;   // (the characters)
            const glm::mat4& mat = sgs.amatGlob[i];
            for (size_t k = 0; k < pg->avtx.size(); k += 2)
            {
                const VTXGLOB& v = pg->avtx[k];
                const float d = glm::length(glm::vec3(mat * glm::vec4(v.pos, 1.0f)) - pos);
                if (d > sReach) continue;
                const float w = 1.0f / (d + 50.0f);
                sum += w * glm::vec3(float(v.rgbaA & 0xFF), float((v.rgbaA >> 8) & 0xFF), float((v.rgbaA >> 16) & 0xFF));
                wSum += w;
            }
        }
    }
    return wSum > 0.0f ? sum / wSum : glm::vec3(128.0f);
}

glm::mat4 MatOfProp(const PROP& p)
{
    glm::mat4 m = glm::translate(glm::mat4(1.0f), p.pos);
    m = glm::rotate(m, glm::radians(p.turn), glm::vec3(0.0f, 0.0f, 1.0f));
    m = glm::rotate(m, glm::radians(p.pitch), glm::vec3(1.0f, 0.0f, 0.0f));
    m = glm::rotate(m, glm::radians(p.roll), glm::vec3(0.0f, 1.0f, 0.0f));
    return glm::scale(m, glm::vec3(p.scale));
}

bool FBuildProp(const SCENE& scene, PROP& prop)
{
    PROPMODEL* pm = PmodelOf(prop.file);
    if (!pm) return false;
    const glm::vec3 rgb = RgbLevelLightAt(scene, prop.pos + glm::vec3(0.0f, 0.0f, pm->sphere.z * prop.scale));
    const glm::mat3 matRot = glm::mat3(MatOfProp(prop));
    auto pgs = std::make_shared<GLOBSET>();
    GLOB glob;
    glob.meshk = MESHK_Strips;
    glob.rp = int8_t(pm->fBlend ? 1 : 3);
    glob.rtck = 2;   // (no billboard: a GLOB's 0 turns it to face the camera)
    glob.posCenter = glm::vec3(pm->sphere);
    glob.sRadius = pm->sphere.w;
    for (size_t ip = 0; ip < pm->aprim.size(); ++ip)
    {
        const GLTFPRIM& p = pm->aprim[ip];
        SUBGLOB sub;
        sub.pshd = pm->apshdPrim[ip];
        sub.ivtxFirst = uint32_t(glob.avtx.size());
        sub.cvtx = uint32_t(p.apos.size());
        sub.firstIndex = uint32_t(glob.aindex.size());
        sub.indexCount = uint32_t(p.aidx.size());
        for (size_t v = 0; v < p.apos.size(); ++v)
        {
            VTXGLOB vx;
            vx.pos = p.apos[v];
            vx.normal = p.anrm[v];
            vx.uv = p.auv[v];
            // the place's light, a little shading by the surface's turn to the sky (the form reads), times the part's own
            // colour (its material's, or painted vertex colours); a model exported from the game keeps its vertices'
            // own colours (their light and tint baked in: its textures are often grey, tinted by them)
            const float shade = 0.78f + 0.28f * std::max(0.0f, glm::normalize(matRot * p.anrm[v]).z);
            const bool fCol = p.acol.size() == p.apos.size();
            glm::vec4 col(rgb * shade * p.rgbFactor, 128.0f);
            if (fCol && p.fColGame) col = p.acol[v] * 128.0f;
            else if (fCol) col = glm::vec4(glm::vec3(col) * glm::vec3(p.acol[v]), p.acol[v].a * 128.0f);
            uint32_t c = 0;
            for (int k = 0; k < 4; ++k) c |= uint32_t(std::clamp(col[k], 0.0f, 255.0f)) << (8 * k);
            vx.rgbaA = vx.rgbaB = vx.rgbaSrc = c;
            glob.avtx.push_back(vx);
        }
        for (uint32_t ix : p.aidx) glob.aindex.push_back(sub.ivtxFirst + ix);
        glob.asubglob.push_back(sub);
    }
    pgs->aglob.push_back(std::move(glob));
    BuildGlobsetGpu(*pgs);
    prop.sgs = SCENEGLOBSET{};
    prop.sgs.pglobset = pgs;
    prop.sgs.oid = -2;
    prop.sgs.amatGlob.assign(1, glm::mat4(1.0f));
    prop.sgs.afVisible.assign(1, 1);
    prop.fBuilt = true;
    return true;
}

bool FShown(const PROP& p)
{
    if (p.after <= 0) return true;
    return NRef(RefGetProgress({"episode", s_aszEpisode[std::clamp(p.after, 1, 8)], "percent_complete"})) >= 100;
}
}  // namespace

void CollectPropDraws(const SCENE& scene, std::vector<EXTRADRAW>& aextra)
{
    if (s_iwrecProps != g_transition.iwrecCur) LoadWorldProps();
    ++s_cframe;
    s_pscene = &scene;
    for (size_t i = 0; i < s_aprop.size(); ++i)
    {
        PROP& p = *s_aprop[i];
        if (!FShown(p) && int(i) != s_ipropSel) continue;
        if (!p.fBuilt && !FBuildProp(scene, p)) continue;
        PROPMODEL* pm = PmodelOf(p.file);
        if (!pm) continue;
        EXTRADRAW x;
        x.psgs = &p.sgs;
        x.iglob = 0;
        x.mat = s_insp.fOpen && int(i) == s_insp.iprop ? MatInspect(p, pm->sphere) : MatOfProp(p);
        // (the selected one, in the placement tool: a slow swell about its middle, drawn opaque: a pulse of its alpha drew
        // it as a fading, see-through object, which blinked out and in)
        if (int(i) == s_ipropSel && g_fCrosshairHit && !(s_insp.fOpen && int(i) == s_insp.iprop))
        {
            const float k = 1.0f + 0.04f * (0.5f + 0.5f * std::sin(float(s_cframe) * 0.12f));
            const glm::vec3 c(pm->sphere);
            x.mat = x.mat * glm::translate(glm::mat4(1.0f), c) * glm::scale(glm::mat4(1.0f), glm::vec3(k)) * glm::translate(glm::mat4(1.0f), -c);
        }
        x.fBounds = true;
        x.posBounds = glm::vec3(x.mat * glm::vec4(glm::vec3(pm->sphere), 1.0f));
        x.sBoundsRadius = pm->sphere.w * p.scale * 1.05f;
        aextra.push_back(x);
    }
}

// ---- the placement tool
void RescanPropModels()
{
    s_fModelsScanned = false;
    for (auto& [file, pm] : s_mpmodel) s_apmodelOld.push_back(std::move(pm));
    s_mpmodel.clear();
    for (auto& p : s_aprop) p->fBuilt = false;
}

int CPropModels()
{
    if (!s_fModelsScanned)
    {
        s_fModelsScanned = true;
        s_afileModel.clear();
        std::error_code ec;
        if (!g_disc.DirMods().empty())
            for (const auto& e : fs::directory_iterator(DirProps() / "Models", ec))
            {
                std::string ext = cane::utf8(e.path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (e.is_regular_file() && (ext == ".glb" || ext == ".gltf")) s_afileModel.push_back(cane::utf8(e.path().filename()));
            }
        std::sort(s_afileModel.begin(), s_afileModel.end());
    }
    return int(s_afileModel.size());
}

std::string StrPropModel(int imodel) { return imodel >= 0 && imodel < CPropModels() ? s_afileModel[size_t(imodel)] : std::string("(none)"); }
int CProps() { if (s_iwrecProps != g_transition.iwrecCur) LoadWorldProps(); return int(s_aprop.size()); }
int IpropSelected() { return s_ipropSel; }
void SelectProp(int iprop) { s_ipropSel = CProps() ? ((iprop % CProps()) + CProps()) % CProps() : -1; }

void SelectPropNearest(const glm::vec3& pos)
{
    float dBest = 1e30f;
    for (size_t i = 0; i < s_aprop.size(); ++i)
        if (const float d = glm::length(s_aprop[i]->pos - pos); d < dBest) dBest = d, s_ipropSel = int(i);
}

std::string StrPropSelected()
{
    if (s_ipropSel < 0 || s_ipropSel >= int(s_aprop.size())) return "(none)";
    const PROP& p = *s_aprop[size_t(s_ipropSel)];
    char ach[320];
    std::snprintf(ach, sizeof ach, "%d/%zu %s  at %.0f %.0f %.0f  turn %.0f  tilt %.0f %.0f  x%.2f  %s%s%s", s_ipropSel + 1, s_aprop.size(),
                  p.file.c_str(), p.pos.x, p.pos.y, p.pos.z, p.turn, p.pitch, p.roll, p.scale, p.after ? "after episode " : "always",
                  p.after ? std::to_string(p.after).c_str() : "", p.fLocked ? "  LOCKED" : p.fBoss ? "  BOSS" : "");
    return ach;
}

bool FPlaceProp(int imodel, const glm::vec3& pos, float turnDeg)
{
    if (imodel < 0 || imodel >= CPropModels()) return false;
    CProps();
    auto prop = std::make_unique<PROP>();
    prop->file = s_afileModel[size_t(imodel)];
    prop->pos = pos;
    prop->turn = std::round(turnDeg / 15.0f) * 15.0f;
    s_aprop.push_back(std::move(prop));
    s_ipropSel = int(s_aprop.size()) - 1;
    return true;
}

namespace
{
PROP* PpropSel() { return s_ipropSel >= 0 && s_ipropSel < int(s_aprop.size()) ? s_aprop[size_t(s_ipropSel)].get() : nullptr; }
// the selected one, unless locked
PROP* PpropEdit() { PROP* p = PpropSel(); return p && !p->fLocked ? p : nullptr; }
float AngleWrap(float a) { a = std::fmod(a, 360.0f); if (a > 180.0f) a -= 360.0f; if (a <= -180.0f) a += 360.0f; return a; }

// the level's solid triangles near pos (the world's space): not the characters, the PS2's shadow shapes or the lights'
// volumes, nor what is hidden now
template <class F>
void ForEachLevelTri(const SCENE& scene, const glm::vec3& pos, float sReach, F&& f)
{
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.pglobset || sgs.irtgt >= 0 || sgs.fForeground || sgs.fActorTree) continue;
        const GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size() && i < sgs.amatGlob.size(); ++i)
        {
            if (i < sgs.afVisible.size() && !sgs.afVisible[i]) continue;
            const GLOB& glob = gs.aglob[i];
            const GLOB* pg = &glob;
            if (pg->avtx.empty() && pg->iglobTemplate >= 0 && size_t(pg->iglobTemplate) < gs.aglob.size()) pg = &gs.aglob[size_t(pg->iglobTemplate)];
            if (pg->meshk != MESHK_Strips || pg->avtx.empty() || pg->aindex.empty() || !pg->abone.empty() || !glob.abone.empty()) continue;
            const int kind = glob.rp & 0x1F;
            if (kind == 4 || kind == 6) continue;   // (the characters)
            const glm::mat4& mat = sgs.amatGlob[i];
            const float sScale = std::max({glm::length(glm::vec3(mat[0])), glm::length(glm::vec3(mat[1])), glm::length(glm::vec3(mat[2]))});
            if (glm::length(glm::vec3(mat * glm::vec4(pg->posCenter, 1.0f)) - pos) > pg->sRadius * sScale + sReach) continue;
            for (const SUBGLOB& sub : pg->asubglob)
            {
                if (sub.pshd && (sub.pshd->shdk == 2 || sub.pshd->shdk == 3)) continue;
                if (size_t(sub.firstIndex) + sub.indexCount > pg->aindex.size()) continue;
                for (uint32_t k = 0; k + 2 < sub.indexCount; k += 3)
                {
                    const uint32_t ia = pg->aindex[sub.firstIndex + k], ib = pg->aindex[sub.firstIndex + k + 1], ic = pg->aindex[sub.firstIndex + k + 2];
                    if (ia >= pg->avtx.size() || ib >= pg->avtx.size() || ic >= pg->avtx.size()) continue;
                    f(glm::vec3(mat * glm::vec4(pg->avtx[ia].pos, 1.0f)), glm::vec3(mat * glm::vec4(pg->avtx[ib].pos, 1.0f)),
                      glm::vec3(mat * glm::vec4(pg->avtx[ic].pos, 1.0f)));
                }
            }
        }
    }
}

// the point of triangle abc nearest p (Ericson, Real-Time Collision Detection 5.1.5)
glm::vec3 PosClosestOnTri(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// the prop's tilt (pitch about X, roll about Y, after its turn about Z: MatOfProp) that stands its up axis along n
void TiltToNormal(PROP& p, glm::vec3 n)
{
    n = glm::normalize(n);
    const float t = glm::radians(p.turn);
    // n in the turned frame: Rz(-turn) n
    const glm::vec3 nl(std::cos(t) * n.x + std::sin(t) * n.y, -std::sin(t) * n.x + std::cos(t) * n.y, n.z);
    // Rx(pitch) Ry(roll) (0, 0, 1) = (sin roll, -sin pitch cos roll, cos pitch cos roll)
    const float roll = std::asin(std::clamp(nl.x, -1.0f, 1.0f));
    const float pitch = std::atan2(-nl.y, nl.z);
    p.pitch = std::round(glm::degrees(pitch) * 10.0f) / 10.0f;
    p.roll = std::round(glm::degrees(roll) * 10.0f) / 10.0f;
}
}

void MovePropSelected(const glm::vec3& pos) { if (PROP* p = PpropEdit()) p->pos = pos, p->fBuilt = false; }
void TurnPropSelected(float dDeg) { if (PROP* p = PpropEdit()) p->turn = std::fmod(p->turn + dDeg + 360.0f, 360.0f), p->fBuilt = false; }
void TiltPropSelected(float dPitch, float dRoll)
{
    if (PROP* p = PpropEdit()) p->pitch = AngleWrap(p->pitch + dPitch), p->roll = AngleWrap(p->roll + dRoll), p->fBuilt = false;
}
void StraightenPropSelected() { if (PROP* p = PpropEdit()) p->pitch = p->roll = 0.0f, p->fBuilt = false; }
void ScalePropSelected(float r) { if (PROP* p = PpropEdit()) p->scale = std::clamp(p->scale * r, 0.01f, 100.0f), p->fBuilt = false; }
void RaisePropSelected(float dz) { if (PROP* p = PpropEdit()) p->pos.z += dz, p->fBuilt = false; }
void CyclePropSelectedAfter() { if (PROP* p = PpropEdit()) p->after = (p->after + 1) % 9; }
bool FPropSelectedLocked() { const PROP* p = PpropSel(); return p && p->fLocked; }
void ToggleLockPropSelected() { if (PROP* p = PpropSel()) p->fLocked = !p->fLocked; }

bool FDropPropSelected(std::string* pstr)
{
    PROP* p = PpropEdit();
    if (!p || !s_pscene) { *pstr = p ? "no level yet" : "nothing selected (or locked)"; return false; }
    // straight down from a little above its base (one sunk in the floor comes up), the nearest surface under it
    const glm::vec3 org = p->pos + glm::vec3(0.0f, 0.0f, 30.0f);
    const float sMax = 5000.0f;
    float tBest = sMax;
    ForEachLevelTri(*s_pscene, org - glm::vec3(0.0f, 0.0f, sMax * 0.5f), sMax * 0.5f, [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
        // (Moller-Trumbore, the ray (0, 0, -1), either side)
        const glm::vec3 e1 = b - a, e2 = c - a, dir(0.0f, 0.0f, -1.0f);
        const glm::vec3 h = glm::cross(dir, e2);
        const float det = glm::dot(e1, h);
        if (std::fabs(det) < 1e-8f) return;
        const float inv = 1.0f / det;
        const glm::vec3 s = org - a;
        const float u = glm::dot(s, h) * inv;
        if (u < 0.0f || u > 1.0f) return;
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(dir, q) * inv;
        if (v < 0.0f || u + v > 1.0f) return;
        const float t = glm::dot(e2, q) * inv;
        if (t > 0.0f && t < tBest) tBest = t;
    });
    if (tBest >= sMax) { *pstr = "no surface below it within 50 m"; return false; }
    p->pos.z = org.z - tBest;
    p->fBuilt = false;
    char ach[96];
    std::snprintf(ach, sizeof ach, "dropped onto the surface below (z %.0f)", p->pos.z);
    *pstr = ach;
    return true;
}

bool FStickPropSelected(bool fStand, std::string* pstr)
{
    PROP* p = PpropEdit();
    if (!p || !s_pscene) { *pstr = p ? "no level yet" : "nothing selected (or locked)"; return false; }
    // the nearest point of the level to its base within 5 m (its face's normal: the side the prop was on)
    const float sReach = 500.0f;
    float dBest = sReach;
    glm::vec3 posBest(0.0f), nrmBest(0.0f, 0.0f, 1.0f);
    ForEachLevelTri(*s_pscene, p->pos, sReach, [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
        const glm::vec3 q = PosClosestOnTri(p->pos, a, b, c);
        const float d = glm::length(q - p->pos);
        if (d >= dBest) return;
        glm::vec3 n = glm::cross(b - a, c - a);
        if (glm::dot(n, n) < 1e-10f) return;
        n = glm::normalize(n);
        const glm::vec3 side = p->pos - q;
        if (glm::dot(side, side) > 1e-4f ? glm::dot(n, side) < 0.0f : n.z < 0.0f) n = -n;
        dBest = d, posBest = q, nrmBest = n;
    });
    if (dBest >= sReach) { *pstr = "no surface within 5 m of it"; return false; }
    p->pos = posBest;
    if (fStand) TiltToNormal(*p, nrmBest);
    p->fBuilt = false;
    char ach[128];
    std::snprintf(ach, sizeof ach, "on the nearest surface, %.0f cm away%s", dBest, fStand ? ", stood along it" : "");
    *pstr = ach;
    return true;
}

bool FPropSelectedBoss() { const PROP* p = PpropSel(); return p && p->fBoss; }
void ToggleBossPropSelected() { if (PROP* p = PpropSel()) p->fBoss = !p->fBoss; }

void DeletePropSelected()
{
    if (!PpropEdit()) return;
    s_insp = INSPECT{};
    s_aprop.erase(s_aprop.begin() + s_ipropSel);
    s_ipropSel = s_aprop.empty() ? -1 : std::min(s_ipropSel, int(s_aprop.size()) - 1);
}

bool FSaveProps(std::string* pstr)
{
    if (g_disc.DirMods().empty()) { *pstr = "no mods folder"; return false; }
    std::error_code ec;
    fs::create_directories(DirProps(), ec);
    std::ofstream out(PathWorldProps());
    if (!out) { *pstr = "cannot write " + cane::utf8(PathWorldProps()); return false; }
    out << "# Sly 2 props of world " << g_transition.iwrecCur << " (Mods/Props/Models' files; the game's units, Z up)\n";
    out << "# <model> <x> <y> <z> <turn> [scale <s>] [tilt <pitch> <roll>] [after <episode>] [locked] [boss] [title <words_with_underscores>] [turnlimit <deg> <deg>]\n";
    out << "# view <name> <x> <y> <z> <yaw> <pitch> [fov]   (\"trophies\": the safe house's trophy stop)\n";
    for (const auto& [name, v] : s_mpview)
    {
        char ach[256];
        std::snprintf(ach, sizeof ach, "view %s %.1f %.1f %.1f %.1f %.1f %.1f\n", name.c_str(), v.pos.x, v.pos.y, v.pos.z, v.yaw, v.pitch, v.fov);
        out << ach;
    }
    for (const auto& pp : s_aprop)
    {
        const PROP& p = *pp;
        char ach[512];
        std::snprintf(ach, sizeof ach, "%s %.1f %.1f %.1f %.1f", p.file.c_str(), p.pos.x, p.pos.y, p.pos.z, p.turn);
        out << ach;
        if (p.scale != 1.0f) out << " scale " << p.scale;
        if (p.pitch != 0.0f || p.roll != 0.0f) out << " tilt " << p.pitch << " " << p.roll;
        if (p.after) out << " after " << p.after;
        if (p.fLocked) out << " locked";
        if (p.fBoss) out << " boss";
        if (!p.title.empty()) out << " title " << p.title;
        if (p.yawLimit >= 0.0f || p.pitchLimit >= 0.0f) out << " turnlimit " << p.yawLimit << " " << p.pitchLimit;
        out << "\n";
    }
    *pstr = "saved " + std::to_string(s_aprop.size()) + " props" + (s_mpview.empty() ? "" : " and " + std::to_string(s_mpview.size()) + " view(s)") +
            ": Mods/Props/" + cane::utf8(PathWorldProps().filename());
    return bool(out);
}

// ---- the world's views
bool FPropViewSet(const char* name)
{
    CProps();   // (this world's file read)
    return s_mpview.count(name) != 0;
}

void SetPropViewHere(const char* name)
{
    if (!s_fCamLast) return;
    CProps();
    const glm::vec3 d = glm::normalize(s_camLast.Dir());
    PROPVIEW v;
    v.pos = s_camLast.pos;
    v.yaw = std::round(glm::degrees(std::atan2(d.y, d.x)) * 10.0f) / 10.0f;
    v.pitch = std::round(glm::degrees(std::asin(std::clamp(d.z, -1.0f, 1.0f))) * 10.0f) / 10.0f;
    v.fov = std::round(s_camLast.fovY * 10.0f) / 10.0f;
    s_mpview[name] = v;
}

void ClearPropView(const char* name)
{
    CProps();
    s_mpview.erase(name);
}

void ApplySafeHouseView(CAMERA& cam, bool fFreeCam)
{
    static double s_tPrev = 0.0;
    static float s_u = 0.0f;   // (0: the game's camera, 1: the view's)
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const float dt = float(std::clamp(t - s_tPrev, 0.0, 0.1));
    s_tPrev = t;
    const PROPVIEW* pv = nullptr;
    if (s_iwrecProps == g_transition.iwrecCur)
        if (auto it = s_mpview.find(SZ_TROPHY_VIEW); it != s_mpview.end()) pv = &it->second;
    const bool fOn = pv && !fFreeCam && FSafeHouseTrophyStop();
    s_u = std::clamp(s_u + (fOn ? dt : -dt) / 0.6f, 0.0f, 1.0f);
    if (fFreeCam || !pv) s_u = 0.0f;
    if (s_u > 0.0f && pv)
    {
        // eased: the game camera's place and heading to the view's (the shorter way round)
        const float u = s_u * s_u * (3.0f - 2.0f * s_u);
        const glm::vec3 d = glm::normalize(cam.Dir());
        const float yaw0 = glm::degrees(std::atan2(d.y, d.x)), pitch0 = glm::degrees(std::asin(std::clamp(d.z, -1.0f, 1.0f)));
        float dyaw = std::fmod(pv->yaw - yaw0, 360.0f);
        if (dyaw > 180.0f) dyaw -= 360.0f;
        if (dyaw < -180.0f) dyaw += 360.0f;
        cam.pos = glm::mix(cam.pos, pv->pos, u);
        cam.yaw = yaw0 + dyaw * u;
        cam.pitch = glm::mix(pitch0, pv->pitch, u);
        if (pv->fov > 1.0f) cam.fovY = glm::mix(cam.fovY, pv->fov, u);
        cam.fMat = false;
    }
    s_camLast = cam;
    s_fCamLast = true;
}

// ---- boss items looked at closely
namespace
{
std::vector<int> AipropBoss()
{
    std::vector<int> a;
    for (size_t i = 0; i < s_aprop.size(); ++i)
        if (s_aprop[i]->fBoss && FShown(*s_aprop[i])) a.push_back(int(i));
    return a;
}

// the camera's frame: forward, left, up (columns)
glm::mat3 MatCameraFrame()
{
    const glm::vec3 f = glm::normalize(s_camLast.Dir());
    glm::vec3 l = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), f);
    l = glm::dot(l, l) < 1e-6f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::normalize(l);
    return glm::mat3(f, l, glm::cross(f, l));
}

void StartInspect(int iprop)
{
    s_insp.iprop = iprop;
    s_insp.fOpen = true;
    s_insp.fClosing = false;
    s_insp.u = 0.0f;
    s_insp.zoom = 1.0f;
    s_insp.tIdle = 0.0f;
    s_insp.yaw = s_insp.pitch = s_insp.tSway = 0.0f;
    const PROP& p = *s_aprop[size_t(iprop)];
    glm::mat4 m = glm::rotate(glm::mat4(1.0f), glm::radians(p.turn), glm::vec3(0.0f, 0.0f, 1.0f));
    m = glm::rotate(m, glm::radians(p.pitch), glm::vec3(1.0f, 0.0f, 0.0f));
    m = glm::rotate(m, glm::radians(p.roll), glm::vec3(0.0f, 1.0f, 0.0f));
    s_insp.q0 = s_insp.q = glm::normalize(glm::quat_cast(glm::transpose(MatCameraFrame()) * glm::mat3(m)));
}

glm::mat4 MatInspect(const PROP& p, const glm::vec4& sphere)
{
    // eased from its place to before the camera, floating; turned about its middle
    const float e = s_insp.u * s_insp.u * (3.0f - 2.0f * s_insp.u);
    const glm::mat3 c = MatCameraFrame();
    const float r = std::max(sphere.w * p.scale, 1.0f);
    const float tanHalf = std::tan(glm::radians(std::clamp(s_camLast.fovY, 10.0f, 120.0f)) * 0.5f);
    const float dist = std::max(r / tanHalf * 1.35f * s_insp.zoom, s_camLast.sNear + r + 5.0f);
    const glm::vec3 posFront = s_camLast.pos + c[0] * dist + c[2] * (std::sin(s_insp.t * 1.6f) * r * 0.06f);
    const glm::vec3 posHome = glm::vec3(MatOfProp(p) * glm::vec4(glm::vec3(sphere), 1.0f));
    const glm::vec3 pos = glm::mix(posHome, posFront, e);
    const glm::mat3 rot = c * glm::mat3_cast(glm::slerp(s_insp.q0, s_insp.q, e));
    glm::mat4 m(rot);
    m[3] = glm::vec4(pos, 1.0f);
    return glm::translate(glm::scale(m, glm::vec3(p.scale)), -glm::vec3(sphere));
}
}  // namespace

int CBossItems()
{
    CProps();
    return int(AipropBoss().size());
}

bool FBossInspectOpen() { return s_insp.fOpen && s_insp.iprop >= 0 && s_insp.iprop < int(s_aprop.size()); }

void OpenBossInspect()
{
    const std::vector<int> a = AipropBoss();
    if (a.empty() || !s_fCamLast) return;
    int i = a[0];
    if (std::find(a.begin(), a.end(), s_insp.iprop) != a.end()) i = s_insp.iprop;   // (the one looked at last)
    s_insp.tPrev = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    StartInspect(i);
}

void CloseBossInspect()
{
    const int i = s_insp.iprop;
    s_insp = INSPECT{};
    s_insp.iprop = i;
}

void UpdateBossInspect()
{
    if (!FBossInspectOpen()) return;
    const double tNow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const float dt = float(std::clamp(tNow - s_insp.tPrev, 0.0, 0.1));
    s_insp.tPrev = tNow;
    s_insp.t += dt;
    if (s_insp.fClosing)
    {
        s_insp.u = std::max(0.0f, s_insp.u - dt / 0.35f);
        if (s_insp.u <= 0.0f) CloseBossInspect();
        return;
    }
    s_insp.u = std::min(1.0f, s_insp.u + dt / 0.45f);
    // circle (or the menu's back): put it back; L1 / R1: the previous / next boss item
    if (GrfbtnTakePressed(BtnMenuBack() | 0x20)) { s_insp.fClosing = true; PsndStartSound(0x35); return; }
    if (const uint32_t grf = GrfbtnTakePressed(0x04 | 0x08))
    {
        const std::vector<int> a = AipropBoss();
        auto it = std::find(a.begin(), a.end(), s_insp.iprop);
        const int k = it == a.end() ? 0 : int(it - a.begin());
        const int c = int(a.size());
        if (c > 1)
        {
            StartInspect(a[size_t((k + ((grf & 0x08) ? 1 : -1) + c) % c)]);
            PsndStartSound(0x35);
        }
        return;
    }
    // the left stick turns it about the camera's up and left axes; the right stick's up / down: nearer / further
    auto dz = [](float g) { return std::fabs(g) < 0.2f ? 0.0f : g; };
    const float xl = dz(g_joy.xLeft), yl = dz(g_joy.yLeft), yr = dz(g_joy.yRight);
    if (xl != 0.0f || yl != 0.0f || yr != 0.0f) s_insp.tIdle = 0.0f;
    else s_insp.tIdle += dt;
    const PROP& prop = *s_aprop[size_t(s_insp.iprop)];
    s_insp.yaw -= xl * 150.0f * dt;
    s_insp.pitch += yl * 150.0f * dt;
    if (s_insp.tIdle > 2.5f)
    {
        // left alone: it turns slowly round, or with a limit sways from side to side within it
        if (prop.yawLimit < 0.0f) s_insp.yaw += 20.0f * dt;
        else
        {
            s_insp.tSway += dt;
            const float yawTo = prop.yawLimit * 0.8f * std::sin(s_insp.tSway * 0.45f);
            s_insp.yaw += (yawTo - s_insp.yaw) * std::min(1.0f, dt * 1.2f);
        }
        if (prop.pitchLimit >= 0.0f) s_insp.pitch -= s_insp.pitch * std::min(1.0f, dt * 0.8f);
    }
    else s_insp.tSway = std::asin(std::clamp(prop.yawLimit > 0.0f ? s_insp.yaw / (prop.yawLimit * 0.8f) : 0.0f, -1.0f, 1.0f)) / 0.45f;
    if (prop.yawLimit >= 0.0f) s_insp.yaw = std::clamp(s_insp.yaw, -prop.yawLimit, prop.yawLimit);
    else s_insp.yaw = std::fmod(s_insp.yaw, 360.0f);
    if (prop.pitchLimit >= 0.0f) s_insp.pitch = std::clamp(s_insp.pitch, -prop.pitchLimit, prop.pitchLimit);
    else s_insp.pitch = std::fmod(s_insp.pitch, 360.0f);
    s_insp.q = glm::normalize(glm::angleAxis(glm::radians(s_insp.yaw), glm::vec3(0.0f, 0.0f, 1.0f)) *
                              glm::angleAxis(glm::radians(s_insp.pitch), glm::vec3(0.0f, 1.0f, 0.0f)) * s_insp.q0);
    s_insp.zoom = std::clamp(s_insp.zoom * std::exp(-yr * 1.5f * dt), 0.45f, 2.5f);
}

std::string StrBossInspected()
{
    if (!FBossInspectOpen()) return "";
    const PROP& p = *s_aprop[size_t(s_insp.iprop)];
    std::string name = p.title;
    if (name.empty())
    {
        name = cane::utf8(cane::path(p.file).stem());
        if (!name.empty()) name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
    }
    std::replace(name.begin(), name.end(), '_', ' ');
    const std::vector<int> a = AipropBoss();
    auto it = std::find(a.begin(), a.end(), s_insp.iprop);
    if (a.size() > 1 && it != a.end()) name += "   " + std::to_string(int(it - a.begin()) + 1) + " / " + std::to_string(a.size());
    return name;
}

void CycleBossLimitSelected(bool fTilt)
{
    PROP* p = PpropSel();
    if (!p) return;
    static const float s_ayaw[] = {-1.0f, 15.0f, 30.0f, 45.0f, 60.0f, 90.0f, 135.0f};
    static const float s_apitch[] = {-1.0f, 0.0f, 10.0f, 20.0f, 30.0f, 45.0f, 60.0f};
    const float* af = fTilt ? s_apitch : s_ayaw;
    float& f = fTilt ? p->pitchLimit : p->yawLimit;
    size_t i = 0;
    while (i < 7 && af[i] != f) ++i;
    f = af[(i + 1) % 7];
}

std::string StrBossLimitSelected(bool fTilt)
{
    const PROP* p = PpropSel();
    if (!p) return "";
    const float f = fTilt ? p->pitchLimit : p->yawLimit;
    if (f < 0.0f) return "Free";
    char ach[32];
    std::snprintf(ach, sizeof ach, "%g deg each way", f);
    return ach;
}
