// See ninjamoves.h.

#include "ninjamoves.h"

#include "actor.h"
#include "audio.h"
#include "brx.h"
#include "clock.h"
#include "disc.h"
#include "display.h"
#include "emitter.h"
#include "gadget.h"
#include "hud.h"
#include "lo.h"
#include "pccharacter.h"
#include "pcprogress.h"
#include "po.h"
#include "render.h"
#include "so.h"
#include "screenrt.h"
#include "sound.h"
#include "sw.h"
#include "wavfile.h"
#include "../platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <random>
#include <vector>

namespace
{
constexpr uint32_t BTN_R1 = 0x8;   // (the PADCTL's layout, as sly.cpp's)

constexpr float kRailSprint = 1.75f;
constexpr float kClimbCobra = 1.8f;

// The afterimages: a pose every dtGhost of sprinting, each drawn tGhostLife after it was taken, the newest skipped
// until it is tGhostMin old (it would lie on Sly himself); violet (light enough for the night levels), alpha from
// aGhost fading to 0.
constexpr float dtGhost = 0.05f, tGhostLife = 0.3f, tGhostMin = 0.045f;
constexpr float aGhost = 0.5f;
const glm::vec3 rgbGhost(0.14f, 0.07f, 0.28f);
constexpr size_t cGhostMax = 8;

struct GHOST
{
    float t = 0.0f;
    std::vector<GHOSTPART> apart;
};
std::deque<GHOST> s_aghost;
uint32_t s_serialGhost = 0;
float s_tLastGhost = -1e9f;
// (diagnostics: the temporary overlay, DrawGhostDiagHud)
struct GHOSTDIAG { int state = -1; bool fOwned = false; float v = 0.0f; int cCaptured = 0; int cSgs = 0; int cPart = 0; float tLast = -1e9f; };
GHOSTDIAG s_diag;
glm::vec3 s_posLast(0.0f);   // (Sly's place at s_tPos, game time, and his speed then)
float s_tPos = -1e9f, s_vGround = 0.0f;

bool FPrint() { static const bool s_f = std::getenv("SLY2N_NINJAPRINT") != nullptr; return s_f; }

// Rail Sprint's look and sound: when it was last running (real time: a pause ends it), the lines' strength
float s_tRealRail = -1e9f;
float s_uRailLines = 0.0f, s_tRealLinesLast = -1e9f;

// The whoosh: 0.55 s of noise through a band-pass sweeping up and back down, swelling and fading, panned from left to
// right (a thing rushing past); 48 kHz stereo
struct WHOOSH { int rate = 48000; int cch = 2; std::vector<int16_t> asample; };
WHOOSH WhooshMade()
{
    WHOOSH w;
    const int c = int(0.55f * float(w.rate));
    w.asample.resize(size_t(c) * 2);
    std::minstd_rand rng(1234);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    float low = 0.0f, band = 0.0f;
    for (int i = 0; i < c; ++i)
    {
        const float u = float(i) / float(c);
        const float frq = 350.0f + 1700.0f * std::sin(3.14159265f * std::pow(u, 0.8f));   // up, then back down
        const float f = 2.0f * std::sin(3.14159265f * frq / float(w.rate));
        const float q = 0.35f;
        // (a state variable filter)
        const float high = dist(rng) - low - q * band;
        band += f * high;
        low += f * band;
        const float env = u < 0.22f ? std::pow(u / 0.22f, 1.5f) : std::pow((1.0f - u) / 0.78f, 2.2f);
        const float x = band * env * 0.55f;
        const float pan = std::clamp(u * 1.4f - 0.2f, 0.0f, 1.0f);   // left to right
        const float gl = std::cos(pan * 1.5707963f), gr = std::sin(pan * 1.5707963f);
        w.asample[size_t(i) * 2] = int16_t(std::clamp(x * gl * 1.41f, -1.0f, 1.0f) * 32000.0f);
        w.asample[size_t(i) * 2 + 1] = int16_t(std::clamp(x * gr * 1.41f, -1.0f, 1.0f) * 32000.0f);
    }
    return w;
}

// the Mods folder's Sounds/rail_sprint.wav (mono or stereo, any rate), else the made one
const WHOOSH& WhooshRail()
{
    static const WHOOSH s_w = [] {
        if (!g_disc.DirMods().empty())
        {
            const std::filesystem::path path = cane::path(g_disc.DirMods()) / "Sounds" / "rail_sprint.wav";
            std::error_code ec;
            WAVINFO info;
            WHOOSH w;
            if (std::filesystem::is_regular_file(path, ec) && FLoadWav(cane::utf8(path), info, w.asample) && (info.cch == 1 || info.cch == 2) &&
                info.rate > 0 && !w.asample.empty())
            {
                w.rate = info.rate;
                w.cch = info.cch;
                return w;
            }
        }
        return WhooshMade();
    }();
    return s_w;
}

// (never destroyed: no voice of it left for after the audio's shutdown; closed when played out, DrawRailSprintLines)
CPcmStream& StreamWhoosh() { static CPcmStream* s_p = new CPcmStream; return *s_p; }

void PlayRailWhoosh()
{
    const float g = 0.8f * GSoundUserEffects();
    if (g <= 0.0f) return;
    const WHOOSH& w = WhooshRail();
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    CPcmStream& stream = StreamWhoosh();
    if (!stream.FOpen(w.cch, w.rate)) return;   // (again from the start: an earlier one cut)
    stream.SetLevels(g, g);
    stream.FPush(w.asample.data(), w.asample.size() / size_t(w.cch), true);
    stream.Start();
}

// The main Sly as the current player (not a PC Extras guest's moves)
SLY* PslyPlaying(ACTOR* pa)
{
    if (!pa || !pa->palo || !g_psw || pa->palo != PloPoCurrent(g_psw) || FPcCarm(pa->palo)) return nullptr;
    return PslyFromLo(pa->palo);
}

// Sly's globsets in the scene: his object's and its parts' (the cane, the hat, ...), two levels down
void CollectSgs(const SCENE& scene, const ALO* palo, int depth, std::vector<const SCENEGLOBSET*>& apsgs)
{
    if (!palo) return;
    if (const SCENEGLOBSET* psgs = PsgsOfAlo(scene, palo))
        if (std::find(apsgs.begin(), apsgs.end(), psgs) == apsgs.end()) apsgs.push_back(psgs);
    if (depth >= 2) return;
    for (const auto& [ib, plo] : palo->apart)
    {
        (void)ib;
        if (plo && plo->fInWorld && FIsLoDerivedFrom(plo, 0x30))
            CollectSgs(scene, static_cast<const ALO*>(plo), depth + 1, apsgs);
    }
}
}  // namespace

bool FSilentSprint(ACTOR* pa) { return PslyPlaying(pa) && FPowerupOwned(kItemSilentSprint); }

float RRailSprint(ACTOR* pa, const PADCTL* pc)
{
    if (!pc || !(pc->grfbtn & BTN_R1) || !PslyPlaying(pa) || !FPowerupOwned(kItemRailSprint)) return 1.0f;
    // (called while Sly runs along the rope: a start after a break of 0.4 s whooshes)
    if (g_clock.tReal - s_tRealRail > 0.4f) PlayRailWhoosh();
    s_tRealRail = g_clock.tReal;
    return kRailSprint;
}

float RClimbCobra(ACTOR* pa, const PADCTL* pc)
{
    if (!pc || !(pc->grfbtn & BTN_R1) || !PslyPlaying(pa) || !FPowerupOwned(kItemClimbingCobra)) return 1.0f;
    return kClimbCobra;
}

void UpdateSprintGhosts(const SCENE& scene)
{
    SW* psw = g_psw;
    if (!psw) return;
    if (s_serialGhost != psw->serial)
    {
        s_aghost.clear();
        s_serialGhost = psw->serial;
        s_tLastGhost = -1e9f;
    }
    const float t = g_clock.t;
    while (!s_aghost.empty() && (t - s_aghost.front().t > tGhostLife || s_aghost.front().t > t)) s_aghost.pop_front();
    LO* plo = PloPoCurrent(psw);
    ACTOR* pa = plo ? PactorFromLo(plo) : nullptr;
    SLY* psly = PslyPlaying(pa);
    // Sly's speed over the ground from his place a frame ago (his position transform has no velocity of its own: he
    // moves by the step physics, so VelAloPos is zero)
    const glm::vec3 pos = psly ? PosAloCenter(psly->palo) : glm::vec3(0.0f);
    const float dtPos = t - s_tPos;
    const float vGround = psly && dtPos > 0.0f && dtPos < 0.25f ? glm::length(glm::vec2(pos - s_posLast)) / dtPos : s_vGround;
    if (dtPos > 0.0f) { s_posLast = pos; s_tPos = t; s_vGround = vGround; }
    s_diag.state = psly ? psly->state : -1;
    s_diag.fOwned = FPowerupOwned(kItemSilentSprint);
    s_diag.v = vGround;
    if (psly && psly->state == 1) s_diag.tLast = g_clock.tReal;
    if (!psly || psly->state != 1 || !FPowerupOwned(kItemSilentSprint)) return;
    if (t - s_tLastGhost < dtGhost) return;
    if (vGround < 150.0f) return;   // (standing in the sprint state: no trail)
    std::vector<const SCENEGLOBSET*> apsgs;
    CollectSgs(scene, psly->palo, 0, apsgs);
    s_diag.cSgs = int(apsgs.size());
    if (apsgs.empty()) return;
    GHOST g;
    g.t = t;
    for (const SCENEGLOBSET* psgs : apsgs)
    {
        if (!psgs->pglobset) continue;
        GHOSTPART part;
        part.psgs = psgs;
        part.pglobset = psgs->pglobset;
        part.amatGlob = psgs->amatGlob;
        part.afVisible = psgs->afVisible;
        part.aamatBone = psgs->aamatBone;
        g.apart.push_back(std::move(part));
    }
    ++s_diag.cCaptured;
    s_diag.cSgs = int(apsgs.size());
    s_diag.cPart = int(g.apart.size());
    s_aghost.push_back(std::move(g));
    while (s_aghost.size() > cGhostMax) s_aghost.pop_front();
    s_tLastGhost = t;
    if (FPrint()) std::printf("ninja: afterimage at %.2f (%zu globsets, %zu kept)\n", t, apsgs.size(), s_aghost.size());
}

void DrawSprintGhosts(const SCENE& scene, const CAMERA& cam)
{
    if (s_aghost.empty() || !g_psw || s_serialGhost != g_psw->serial) return;
    const float t = g_clock.t;
    std::vector<GHOSTDRAW> adraw;
    for (const GHOST& g : s_aghost)   // (oldest first: the newer ones over them)
    {
        const float age = t - g.t;
        if (age < tGhostMin || age > tGhostLife) continue;
        const float u = 1.0f - (age - tGhostMin) / (tGhostLife - tGhostMin);
        adraw.push_back({&g.apart, glm::vec4(rgbGhost, aGhost * u)});
    }
    RenderGhostPoses(scene, cam, adraw);
}

void DrawRailSprintLines(int width, int height)
{
    // the strength: up in 0.15 s while running, down in 0.3 s after
    const float tReal = g_clock.tReal;
    const float dt = std::clamp(tReal - s_tRealLinesLast, 0.0f, 0.1f);
    s_tRealLinesLast = tReal;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
        if (CPcmStream& stream = StreamWhoosh(); stream.FIsOpen() && stream.CBufferQueued() == 0) stream.Close();
    }
    const bool fOn = tReal - s_tRealRail < 0.12f;
    s_uRailLines = std::clamp(s_uRailLines + (fOn ? dt / 0.15f : -dt / 0.3f), 0.0f, 1.0f);
    if (s_uRailLines <= 0.0f) return;
    if (static const bool s_fOff = [] { const char* sz = std::getenv("SLY2N_RAILLINES"); return sz && sz[0] == '0'; }(); s_fOff) return;

    // the picture's canvas range (wider than 640 on a wide picture)
    const glm::vec2 s = g_display.uiScale;
    const float hx = 320.0f / s.x, hy = 224.0f / s.y;
    const glm::vec2 c(320.0f, 224.0f);
    // new lines 20 times a second (a comic's flicker), the same within a step
    std::minstd_rand rng(uint32_t(tReal * 20.0f) * 2654435761u + 7u);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    g_hud.Begin();
    constexpr int cLine = 26;
    for (int i = 0; i < cLine; ++i)
    {
        // spread round the picture, each in its own sector, jittered
        const float rad = (float(i) + u01(rng)) / float(cLine) * 6.2831853f;
        const glm::vec2 d(std::cos(rad), std::sin(rad));
        // from inside the picture's edge out past it (rEdge: where this direction meets the edge), shorter as they
        // come and go
        const float rEdge = 1.0f / std::max(std::abs(d.x), std::abs(d.y));
        const float r0 = rEdge * (0.62f + 0.22f * u01(rng) + (1.0f - s_uRailLines) * 0.3f);
        const glm::vec2 a = c + glm::vec2(d.x * hx, d.y * hy) * r0;
        const glm::vec2 b = c + glm::vec2(d.x * hx, d.y * hy) * (rEdge * 1.08f);
        const float w = 0.8f + 1.8f * u01(rng);
        const float alpha = (34.0f + 40.0f * u01(rng)) * s_uRailLines;
        g_hud.FillScreenLine(a, b, w, glm::vec4(128, 128, 128, alpha), 0.5f);
    }
    g_hud.Flush(width, height);
}

float DxTextUi(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88);   // splayer.cpp

// TEMPORARY diagnostics for the afterimages (to be removed): while Sly sprints (and 3 s after) a line at the top left
// of the picture: Sly's state, the item owned, his speed, the poses taken, the globsets found, the renderer's program,
// poses, draws and GL error.
void DrawGhostDiagHud(int width, int height)
{
    if (g_clock.tReal - s_diag.tLast > 3.0f) return;
    const FONTBRX* pfont = PfontHud();
    if (!pfont) return;
    const GHOSTRENDERDIAG& r = g_ghostrenderdiag;
    char ach[256];
    std::snprintf(ach, sizeof(ach), "ghost diag: state %d owned %d v %.0f | taken %d sgs %d parts %d kept %zu | gl %d poses %d draws %d err 0x%X frames %d",
                  s_diag.state, s_diag.fOwned ? 1 : 0, s_diag.v, s_diag.cCaptured, s_diag.cSgs, s_diag.cPart, s_aghost.size(), r.state, r.cpose, r.cdraw,
                  r.err, r.cframe);
    g_hud.Begin();
    const float z = 16760000.0f / 16777216.0f;
    g_hud.FillScreenRect(4.0f, 4.0f, 636.0f, 20.0f, glm::vec4(0, 0, 0, 100), z - 1e-4f);
    g_hud.DrawTextBox(*pfont, PfontHudIcons(), ach, 8.0f - 320.0f, (6.0f - 224.0f) * 1.1f, 10.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0,
                      glm::vec4(110, 110, 60, 0), glm::vec4(1.0f), glm::vec4(0.0f), 16760100.0f, false);
    g_hud.Flush(width, height);
}
