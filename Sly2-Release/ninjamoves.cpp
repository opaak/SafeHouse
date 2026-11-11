// See ninjamoves.h.

#include "ninjamoves.h"

#include "actor.h"
#include "audio.h"
#include "brx.h"
#include "clock.h"
#include "cm.h"
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
glm::vec3 s_posLast(0.0f);   // (Sly's place at s_tPos, game time, and his speed then)
float s_tPos = -1e9f, s_vGround = 0.0f;

bool FPrint() { static const bool s_f = std::getenv("SLY2N_NINJAPRINT") != nullptr; return s_f; }

// Rail Sprint's look and sound: when it was last running (real time: a pause ends it), the lines' strength
float s_tRealRail = -1e9f;
ACTOR* s_paRail = nullptr;   // (who runs: his place the streaks follow)
float s_uRailLines = 0.0f, s_tRealLinesLast = -1e9f;

// The wind: a soft, endless wind made as it plays (two channels of noise, each through a gently wandering band-pass,
// swelling in slow gusts), or the Mods folder's Sounds/rail_sprint.wav looped; faded in while Rail Sprint runs and out
// after (UpdateRailWind, each frame)
constexpr int kRateWind = 48000;
constexpr float gWind = 0.3f;    // (at full: under the effects slider; a bed under the game's own sounds)
struct WINDGEN
{
    std::minstd_rand rng{4321};
    float alow[2]{}, aband[2]{}, asoft[2]{};
    float phGust = 0.0f, phWander = 0.0f;

    void Fill(int16_t* asample, int cframe)
    {
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        for (int i = 0; i < cframe; ++i)
        {
            phGust += 0.55f / float(kRateWind);
            phWander += 0.23f / float(kRateWind);
            const float gust = 0.65f + 0.35f * std::sin(6.2831853f * phGust) * std::sin(6.2831853f * phGust * 0.37f + 1.0f);
            for (int ch = 0; ch < 2; ++ch)
            {
                // (low and broad: an airy rush, no whistle; then a gentle low-pass for the hiss)
                const float frq = 230.0f + 110.0f * std::sin(6.2831853f * (phWander + 0.31f * float(ch)));
                const float f = 2.0f * std::sin(3.14159265f * frq / float(kRateWind));
                const float high = dist(rng) - alow[ch] - 1.4f * aband[ch];
                aband[ch] += f * high;
                alow[ch] += f * aband[ch];
                const float raw = (aband[ch] * 0.7f + alow[ch] * 0.35f) * gust;
                asoft[ch] += 0.18f * (raw - asoft[ch]);
                const float x = asoft[ch] * 1.6f;
                asample[i * 2 + ch] = int16_t(std::clamp(x, -1.0f, 1.0f) * 30000.0f);
            }
        }
    }
};

// the Mods folder's Sounds/rail_sprint.wav as stereo 16-bit (empty: none)
struct WINDFILE { int rate = 0; std::vector<int16_t> asample; size_t iframe = 0; };
WINDFILE& WindFile()
{
    static WINDFILE s_wf = [] {
        WINDFILE wf;
        if (g_disc.DirMods().empty()) return wf;
        const std::filesystem::path path = cane::path(g_disc.DirMods()) / "Sounds" / "rail_sprint.wav";
        std::error_code ec;
        WAVINFO info;
        std::vector<int16_t> a;
        if (!std::filesystem::is_regular_file(path, ec) || !FLoadWav(cane::utf8(path), info, a) || info.rate <= 0 || a.empty()) return wf;
        if (info.cch == 2) wf.asample = std::move(a);
        else if (info.cch == 1)
            for (int16_t x : a) { wf.asample.push_back(x); wf.asample.push_back(x); }
        else return wf;
        wf.rate = info.rate;
        return wf;
    }();
    return s_wf;
}

// (never destroyed: no voice of it left for after the audio's shutdown; closed when faded out)
CPcmStream& StreamWind() { static CPcmStream* s_p = new CPcmStream; return *s_p; }
WINDGEN s_windgen;
float s_gWindNow = 0.0f;

void UpdateRailWind(bool fOn, float dt)
{
    s_gWindNow = std::clamp(s_gWindNow + (fOn ? dt / 0.35f : -dt / 0.5f), 0.0f, 1.0f);
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    CPcmStream& stream = StreamWind();
    if (s_gWindNow <= 0.0f)
    {
        if (stream.FIsOpen()) stream.Close();
        return;
    }
    WINDFILE& wf = WindFile();
    const int rate = wf.rate > 0 ? wf.rate : kRateWind;
    if (!stream.FIsOpen())
    {
        if (!stream.FOpen(2, rate)) return;
        stream.Start();
    }
    // 0.1 s buffers, three ahead
    const int cframe = rate / 10;
    std::vector<int16_t> a(size_t(cframe) * 2);
    while (stream.CBufferQueued() < 3)
    {
        if (wf.rate > 0)
        {
            const size_t cf = wf.asample.size() / 2;
            for (int i = 0; i < cframe; ++i, wf.iframe = (wf.iframe + 1) % cf)
            {
                a[size_t(i) * 2] = wf.asample[wf.iframe * 2];
                a[size_t(i) * 2 + 1] = wf.asample[wf.iframe * 2 + 1];
            }
        }
        else s_windgen.Fill(a.data(), cframe);
        if (!stream.FPush(a.data(), size_t(cframe))) break;
    }
    // (eased: a soft start and end)
    const float g = gWind * GSoundUserEffects() * s_gWindNow * s_gWindNow * (3.0f - 2.0f * s_gWindNow);
    stream.SetLevels(g, g);
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
    s_tRealRail = g_clock.tReal;   // (while Sly runs along the rope: the wind and its lines, DrawRailSprintLines)
    s_paRail = pa;
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
    if (!psly || psly->state != 1 || !FPowerupOwned(kItemSilentSprint)) return;
    if (t - s_tLastGhost < dtGhost) return;
    if (vGround < 150.0f) return;   // (standing in the sprint state: no trail)
    std::vector<const SCENEGLOBSET*> apsgs;
    CollectSgs(scene, psly->palo, 0, apsgs);
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

// The wind's lines: thin, faint streaks of air round Sly, following him: each born a little ahead of him at his side,
// above or below, sliding back past him along his way and fading in and out over ~0.4 s (in his frame: they go where
// he goes)
struct WINDLINE { glm::vec2 off{0.0f}; float along = 0.0f, v = 0.0f, len = 0.0f, w = 1.0f, t = 0.0f, life = 0.0f, a = 0.0f; };
std::vector<WINDLINE> s_aline;
float s_tSpawnLine = 0.0f;
std::minstd_rand s_rngLine(99);
glm::vec3 s_posRailLast(0.0f), s_dirRail(1.0f, 0.0f, 0.0f);
float s_tPosRail = -1e9f;

// the canvas point of a world point; false behind the camera
bool FCanvasOfWorld(const glm::vec3& pos, glm::vec2* pc)
{
    const glm::vec3 sc = ConvertCmWorldToScreen(&g_cm, pos);
    if (sc.z <= 1.0f) return false;
    *pc = glm::vec2(((sc.x - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f,
                    (1.0f - (sc.y - g_display.uiOffset.y) / g_display.uiScale.y) * 224.0f);
    return true;
}

void DrawRailSprintLines(int width, int height)
{
    const float tReal = g_clock.tReal;
    const float dt = std::clamp(tReal - s_tRealLinesLast, 0.0f, 0.1f);
    s_tRealLinesLast = tReal;
    const bool fOn = tReal - s_tRealRail < 0.12f;
    UpdateRailWind(fOn, dt);
    s_uRailLines = std::clamp(s_uRailLines + (fOn ? dt / 0.25f : -dt / 0.4f), 0.0f, 1.0f);

    // Sly's place and his way along the rope (from his movement; kept while he stands)
    SLY* psly = s_paRail ? PslyPlaying(s_paRail) : nullptr;
    if (!psly) { s_aline.clear(); return; }
    const glm::vec3 pos = PosAloCenter(s_paRail->palo);
    if (const float dtPos = g_clock.t - s_tPosRail; dtPos > 0.0f)
    {
        const glm::vec3 d = pos - s_posRailLast;
        if (dtPos < 0.25f && glm::length(d) > 0.5f) s_dirRail = glm::normalize(glm::mix(s_dirRail, glm::normalize(d), 0.35f));
        s_posRailLast = pos;
        s_tPosRail = g_clock.t;
    }
    const glm::vec3 dir = s_dirRail;
    glm::vec3 side = glm::cross(dir, glm::vec3(0.0f, 0.0f, 1.0f));
    side = glm::length(side) > 0.1f ? glm::normalize(side) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 up = glm::normalize(glm::cross(side, dir));

    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    // new streaks while it runs: about 14 a second
    if (fOn && !(std::getenv("SLY2N_RAILLINES") && std::getenv("SLY2N_RAILLINES")[0] == '0'))
    {
        s_tSpawnLine -= dt;
        while (s_tSpawnLine <= 0.0f)
        {
            s_tSpawnLine += 0.045f + 0.05f * u01(s_rngLine);
            WINDLINE l;
            // round him, off his body: 45 to 95 out, from his side, above, below
            const float rad = u01(s_rngLine) * 6.2831853f, r = 45.0f + 50.0f * u01(s_rngLine);
            l.off = glm::vec2(std::cos(rad) * r, std::sin(rad) * r * 0.9f);
            l.along = 40.0f + 60.0f * u01(s_rngLine);    // (ahead of him)
            l.v = 380.0f + 220.0f * u01(s_rngLine);      // (sliding back past him)
            l.len = 45.0f + 55.0f * u01(s_rngLine);
            l.w = 0.8f + 0.7f * u01(s_rngLine);
            l.life = 0.32f + 0.18f * u01(s_rngLine);
            l.a = 0.6f + 0.4f * u01(s_rngLine);
            s_aline.push_back(l);
        }
    }
    for (WINDLINE& l : s_aline) { l.t += dt; l.along -= l.v * dt; }
    s_aline.erase(std::remove_if(s_aline.begin(), s_aline.end(), [](const WINDLINE& l) { return l.t >= l.life; }), s_aline.end());
    if (s_aline.empty() || s_uRailLines <= 0.0f) return;

    g_hud.Begin();
    for (const WINDLINE& l : s_aline)
    {
        const glm::vec3 base = pos + side * l.off.x + up * l.off.y;
        const glm::vec3 p0 = base + dir * l.along, p1 = base + dir * (l.along - l.len);
        glm::vec2 a, b;
        if (!FCanvasOfWorld(p0, &a) || !FCanvasOfWorld(p1, &b)) continue;
        const float fade = std::sin(3.14159265f * (l.t / l.life));
        // (a soft line: a faint wide one under a thinner, a little brighter)
        const float alpha = l.a * fade * s_uRailLines;
        g_hud.FillScreenLine(a, b, l.w * 2.2f, glm::vec4(118, 124, 128, 12.0f * alpha), 0.5f);
        g_hud.FillScreenLine(a, b, l.w, glm::vec4(122, 126, 128, 34.0f * alpha), 0.5f);
    }
    g_hud.Flush(width, height);
}
