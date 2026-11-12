// See skillfx.h.
#include "skillfx.h"

#include "activate.h"
#include "alo.h"
#include "ancestors.h"
#include "bsp.h"
#include "brx.h"
#include "clock.h"
#include "cm.h"
#include "display.h"
#include "emitter.h"
#include "homingdarts.h"
#include "hud.h"
#include "lo.h"
#include "po.h"
#include "render.h"
#include "scene.h"
#include "so.h"
#include "sw.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <random>
#include <vector>

namespace
{
std::minstd_rand s_rng(4242);
float Rand(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(s_rng); }

// colours in GS units (0x80 = full; alpha 0x80 = opaque)
const glm::vec4 kPale(110, 118, 128, 110), kDust(96, 88, 74, 70), kOrange(128, 72, 16, 120), kFlame(128, 96, 30, 128);
const glm::vec4 kEmber(120, 34, 8, 110), kBlue(50, 100, 128, 110), kWhite(128, 128, 128, 128), kSpark(128, 110, 60, 128);

// ---- particles: a dot (a disc shrinking or growing) or a streak (a line along its velocity)
struct FXP
{
    glm::vec3 pos, vel;
    float grav = 0.0f, drag = 0.0f;    // (units / s^2 down; the share of the velocity lost a second)
    float t0 = 0.0f, life = 0.5f;
    float size0 = 10.0f, size1 = 0.0f; // world units
    glm::vec4 rgba0, rgba1;            // (the alpha fades to rgba1's)
    bool fStreak = false;
    bool fFlame = false;               // a tongue of flame: size0 its length along dir, size1 its width
    glm::vec3 dir{0.0f, 0.0f, 1.0f};
    float seed = 0.0f;
};
std::vector<FXP> s_ap;

// ---- rings: a circle on the ground growing from r0 to r1
struct FXRING
{
    glm::vec3 pos;
    float r0 = 0, r1 = 100, t0 = 0, life = 0.4f, w = 10.0f;   // w: world units across
    float dz = 0;                                             // (rising: the ring's height at the end)
    glm::vec4 rgba;
};
std::vector<FXRING> s_aring;

float s_tFlash = -10.0f, s_aFlash = 0.0f;
uint32_t s_serial = 0;

void Reset()
{
    s_ap.clear();
    s_aring.clear();
    s_tFlash = -10.0f;
}

bool FStep()
{
    if (!g_psw) return false;
    if (s_serial != g_psw->serial) { Reset(); s_serial = g_psw->serial; }
    return true;
}

void Add(const FXP& p) { if (s_ap.size() < 1200) s_ap.push_back(p); }

FXP Dot(const glm::vec3& pos, const glm::vec3& vel, float life, float s0, float s1, glm::vec4 c0, glm::vec4 c1, float grav = 0, float drag = 0)
{
    FXP p;
    p.pos = pos, p.vel = vel, p.life = life, p.size0 = s0, p.size1 = s1, p.rgba0 = c0, p.rgba1 = c1, p.grav = grav, p.drag = drag;
    p.t0 = g_clock.t;
    return p;
}

// a tongue of flame at pos (moving at vel), pointing along dir: len long, w wide (world units)
void Flame(const glm::vec3& pos, const glm::vec3& vel, const glm::vec3& dir, float life, float len, float w)
{
    FXP p = Dot(pos, vel, life, len, w, glm::vec4(0.0f), glm::vec4(0.0f), 0.0f, 1.5f);
    p.fFlame = true;
    p.dir = glm::length(dir) > 1e-4f ? glm::normalize(dir) : glm::vec3(0, 0, 1);
    p.seed = Rand(0.0f, 100.0f);
    Add(p);
}

void Ring(const glm::vec3& pos, float r0, float r1, float life, float w, glm::vec4 c, float dz = 0.0f, float tDelay = 0.0f)
{
    FXRING r;
    r.pos = pos, r.r0 = r0, r.r1 = r1, r.life = life, r.w = w, r.rgba = c, r.dz = dz;
    r.t0 = g_clock.t + tDelay;
    s_aring.push_back(r);
}

// dust round a point, pushed out flat
void DustRing(const glm::vec3& pos, int c, float v, float size)
{
    for (int i = 0; i < c; ++i)
    {
        const float a = 6.2831853f * (float(i) + Rand(0.0f, 0.6f)) / float(c);
        const glm::vec3 d(std::cos(a), std::sin(a), 0.0f);
        Add(Dot(pos + d * Rand(10.0f, 40.0f) + glm::vec3(0, 0, 8), d * v * Rand(0.6f, 1.0f) + glm::vec3(0, 0, Rand(40.0f, 140.0f)),
                Rand(0.45f, 0.75f), size * 0.6f, size * Rand(1.4f, 2.0f), kDust, glm::vec4(kDust.r, kDust.g, kDust.b, 0), 0.0f, 3.0f));
    }
}

void Sparks(const glm::vec3& pos, int c, float v, glm::vec4 col, float up = 0.4f)
{
    for (int i = 0; i < c; ++i)
    {
        glm::vec3 d(Rand(-1.0f, 1.0f), Rand(-1.0f, 1.0f), Rand(0.0f, 1.0f) + up);
        d = glm::normalize(d);
        FXP p = Dot(pos, d * v * Rand(0.5f, 1.0f), Rand(0.25f, 0.5f), 3.0f, 1.5f, col, glm::vec4(kEmber.r, kEmber.g, kEmber.b, 0), 1400.0f, 1.0f);
        p.fStreak = true;
        Add(p);
    }
}

// ---- the canvas (640 x 448) of a world point; false behind the camera
bool FCanvas(const glm::vec3& pos, glm::vec2* pc)
{
    const glm::vec3 sc = ConvertCmWorldToScreen(&g_cm, pos);
    if (sc.z <= 1.0f) return false;
    *pc = glm::vec2(((sc.x - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f,
                    (1.0f - (sc.y - g_display.uiOffset.y) / g_display.uiScale.y) * 224.0f);
    return true;
}
// a world length at a point in canvas pixels (along the camera's up)
float PixelsOf(const glm::vec3& pos, float s, const glm::vec2& c)
{
    glm::vec2 c2;
    if (!FCanvas(pos + g_cm.mat[2] * s, &c2)) return 0.0f;
    return glm::length(c2 - c);
}

// ---- the ground under a mover (its object record is at its middle, not its feet): straight down from there, past
// its own collision, up to 400 below; none: 70 below
glm::vec3 PosGround(const glm::vec3& pos, ALO* palo)
{
    const glm::vec3 a = pos + glm::vec3(0, 0, 10), b = pos - glm::vec3(0, 0, 400);
    std::vector<SOE*> apsoe;
    LO* proot = palo ? PloRoot(palo) : nullptr;
    for (SOE* pe : AsolidWithTree(a, b))
        if (!proot || PloRoot(pe->palo) != proot) apsoe.push_back(pe);
    LSG lsg;
    if (PsoeHitTestLineObjects(1, a, b, apsoe, &lsg)) return lsg.pos0;
    return pos - glm::vec3(0, 0, 70);
}
glm::vec3 PosFeet(ALO* palo) { return PosGround(PosAloCenter(palo), palo); }

// ---- the Scorching Hover Pack's jet: Bentley's glow parts (drawn added) kept as placed and hidden from the scene
// while he hovers, drawn red instead (as Leaping Dragon's aura)
std::mutex s_mtxHover;
ALO* s_paloHoverRed = nullptr;      // the hovering Bentley (null: none)
ALO* s_paloHoverNoted = nullptr;    // noted this step
std::vector<GHOSTPART> s_aparthover;
const glm::vec3 kRgbHover(1.0f, 0.2f, 0.07f);

bool FUnderAlo(const ALO* palo, const ALO* proot)
{
    for (const ALO* p = palo; p; p = p->paloParent)
        if (p == proot) return true;
    return false;
}
bool FAddedPass(const SUBGLOB& sub)
{
    if (!sub.pshd || sub.pshd->apass.empty()) return false;
    const SHDPASS& ps = sub.pshd->apass[0];
    return ps.fBlend && ((ps.alpha >> 2) & 3) == 2 && ((ps.alpha >> 6) & 3) == 1;
}

// ---- afterimages (Adrenaline Burst Plus), as Silent Sprint's (ninjamoves.cpp)
struct GHOST { float t = 0; std::vector<GHOSTPART> apart; };
std::deque<GHOST> s_aghost;
ALO* s_paloGhost = nullptr;
float s_tGhostNote = -10.0f, s_tGhostLast = -10.0f;
constexpr float kDtGhost = 0.06f, kTGhostLife = 0.32f, kTGhostMin = 0.05f;

void CollectSgs(const SCENE& scene, const ALO* palo, int depth, std::vector<const SCENEGLOBSET*>& apsgs)
{
    if (!palo) return;
    if (const SCENEGLOBSET* psgs = PsgsOfAlo(scene, palo))
        if (std::find(apsgs.begin(), apsgs.end(), psgs) == apsgs.end()) apsgs.push_back(psgs);
    if (depth >= 2) return;
    for (const auto& [ib, plo] : palo->apart)
    {
        (void)ib;
        if (plo && plo->fInWorld && FIsLoDerivedFrom(plo, 0x30)) CollectSgs(scene, static_cast<const ALO*>(plo), depth + 1, apsgs);
    }
}

// the moves noted this step (the hooks), for UpdateSkillFx
ALO* s_paloRocket = nullptr;
ALO* s_paloFeather = nullptr;
float s_tFeatherPuff = -10.0f, s_tAdrenStreak = -10.0f;
glm::vec3 s_posFeatherLast(0.0f), s_posRocketLast(0.0f);
}  // namespace

void SkillFxShockwave(const glm::vec3& posIn, float r, SKILLFXK k, ALO* paloOn)
{
    if (!FStep()) return;
    const glm::vec3 pos = paloOn ? PosGround(posIn, paloOn) : posIn;
    switch (k)
    {
    case SKILLFXK_MegaCharge:
        Ring(pos + glm::vec3(0, 0, 6), 30.0f, r, 0.32f, 14.0f, kPale);
        Ring(pos + glm::vec3(0, 0, 6), 20.0f, r * 0.7f, 0.4f, 6.0f, kWhite, 0.0f, 0.05f);
        DustRing(pos, 12, 420.0f, 26.0f);
        JoltCm(&g_cm, 0);
        break;
    case SKILLFXK_MegaStomp:
        Ring(pos + glm::vec3(0, 0, 6), 40.0f, r, 0.45f, 22.0f, kPale);
        Ring(pos + glm::vec3(0, 0, 6), 30.0f, r * 0.75f, 0.55f, 10.0f, kWhite, 0.0f, 0.08f);
        DustRing(pos, 22, 700.0f, 40.0f);
        DustRing(pos, 10, 300.0f, 30.0f);
        JoltCm(&g_cm, 0);
        break;
    case SKILLFXK_BombBlast:
        Ring(pos, r * 0.2f, r, 0.35f, 16.0f, kOrange);
        Ring(pos, r * 0.1f, r * 0.8f, 0.45f, 8.0f, kFlame, 0.0f, 0.04f);
        Sparks(pos + glm::vec3(0, 0, 20), 18, 900.0f, kSpark, 0.6f);
        break;
    }
}

void SkillFxFlash(const glm::vec3& pos)
{
    if (!FStep()) return;
    glm::vec2 c;
    const bool fSeen = FCanvas(pos, &c) && c.x > -40.0f && c.x < 680.0f && c.y > -40.0f && c.y < 488.0f;
    s_tFlash = g_clock.t;
    s_aFlash = fSeen ? 1.0f : 0.45f;
    Ring(pos + glm::vec3(0, 0, 10), 50.0f, 1000.0f, 0.3f, 18.0f, kWhite);
    Sparks(pos + glm::vec3(0, 0, 60), 24, 700.0f, kWhite, 0.8f);
}

void SkillFxRecharge(ALO* palo)
{
    if (!FStep() || !palo) return;
    const glm::vec3 pos = PosFeet(palo);
    for (int i = 0; i < 3; ++i) Ring(pos + glm::vec3(0, 0, 5), 70.0f, 55.0f, 0.7f, 6.0f, kBlue, 170.0f, 0.12f * float(i));
    for (int i = 0; i < 16; ++i)
    {
        const float a = Rand(0.0f, 6.2831853f);
        Add(Dot(pos + glm::vec3(std::cos(a) * 60.0f, std::sin(a) * 60.0f, Rand(0.0f, 60.0f)), glm::vec3(0, 0, Rand(120.0f, 260.0f)),
                Rand(0.5f, 0.9f), 6.0f, 0.0f, kWhite, glm::vec4(kBlue.r, kBlue.g, kBlue.b, 0)));
    }
}

void SkillFxBurn(const glm::vec3& pos)
{
    if (!FStep()) return;
    Sparks(pos + glm::vec3(0, 0, 60), 10, 450.0f, kSpark, 1.2f);
    for (int i = 0; i < 7; ++i)
        Flame(pos + glm::vec3(Rand(-35.f, 35.f), Rand(-35.f, 35.f), Rand(30.f, 100.f)), glm::vec3(Rand(-20.f, 20.f), Rand(-20.f, 20.f), Rand(70.f, 140.f)),
              glm::vec3(Rand(-0.15f, 0.15f), Rand(-0.15f, 0.15f), 1.0f), Rand(0.4f, 0.65f), Rand(55.f, 95.f), Rand(12.f, 17.f));
}

void SkillFxKnightsLeap(ALO* palo)
{
    if (!FStep() || !palo) return;
    const glm::vec3 pos = PosAloCenter(palo);
    const glm::vec4 kGold(128, 104, 40, 120);
    Ring(pos - glm::vec3(0, 0, 40), 20.0f, 150.0f, 0.3f, 10.0f, kGold);
    Ring(pos - glm::vec3(0, 0, 40), 10.0f, 100.0f, 0.38f, 5.0f, kWhite, 0.0f, 0.05f);
    Sparks(pos - glm::vec3(0, 0, 30), 14, 500.0f, kSpark, -0.6f);
}

// the streak: a few gold motes from the pickup to him, each following him as it flies (drawn at the pickup's place
// moving to his: their velocity set toward him each step in UpdateSkillFx)
struct SKILLMOTE { glm::vec3 pos; ALO* paloTo; float t0; glm::vec4 rgba; };
static std::vector<SKILLMOTE> s_amote;
void SkillFxMagnet(const glm::vec3& pos, ALO* paloTo, bool fClue)
{
    if (!FStep() || !paloTo) return;
    const glm::vec4 c = fClue ? glm::vec4(70, 128, 118, 128) : glm::vec4(128, 110, 40, 128);
    for (int i = 0; i < 5; ++i)
        s_amote.push_back({pos + glm::vec3(Rand(-12.f, 12.f), Rand(-12.f, 12.f), Rand(-12.f, 12.f)), paloTo, g_clock.t + 0.03f * float(i), c});
}

void NoteSkillFxHover(ALO* palo) { s_paloHoverNoted = palo; }

const LO* PloScorchingHover()
{
    std::lock_guard<std::mutex> lock(s_mtxHover);
    return s_paloHoverRed;
}

void NoteSkillFxPlaced(ALO* palo, SCENEGLOBSET& sgs)
{
    if (!s_paloHoverRed || !palo) return;   // (the common case, without the lock)
    std::lock_guard<std::mutex> lock(s_mtxHover);
    if (!s_paloHoverRed || !FUnderAlo(palo, s_paloHoverRed) || !sgs.pglobset) return;
    const GLOBSET& gs = *sgs.pglobset;
    // its groups drawn only added (the jet's glow): kept, and hidden from the scene
    std::vector<uint8_t> afGlow(gs.aglob.size(), 0);
    bool fAny = false;
    for (size_t i = 0; i < gs.aglob.size() && i < sgs.afVisible.size(); ++i)
    {
        if (!sgs.afVisible[i]) continue;
        const GLOB& glob = gs.aglob[i];
        const GLOB* pgeom = !glob.pgl && glob.iglobTemplate >= 0 && size_t(glob.iglobTemplate) < gs.aglob.size() ? &gs.aglob[size_t(glob.iglobTemplate)] : &glob;
        bool fAll = !pgeom->asubglob.empty();
        for (const SUBGLOB& sub : pgeom->asubglob)
            if (sub.indexCount && !FAddedPass(sub)) { fAll = false; break; }
        if (fAll) afGlow[i] = 1, fAny = true;
    }
    if (!fAny) return;
    GHOSTPART* ppart = nullptr;
    for (GHOSTPART& part : s_aparthover)
        if (part.psgs == &sgs) ppart = &part;
    if (!ppart) { s_aparthover.emplace_back(); ppart = &s_aparthover.back(); }
    ppart->psgs = &sgs;
    ppart->pglobset = sgs.pglobset;
    ppart->amatGlob = sgs.amatGlob;
    ppart->afVisible = afGlow;
    ppart->aamatBone = sgs.aamatBone;
    for (size_t i = 0; i < afGlow.size(); ++i)
        if (afGlow[i]) sgs.afVisible[i] = 0;   // (not the scene's: its own colour)
}
void NoteSkillFxRocketSlide(ALO* palo) { s_paloRocket = palo; }
void NoteSkillFxAdrenaline(ALO* palo) { s_paloGhost = palo; s_tGhostNote = g_clock.t; }
void NoteSkillFxFeather(ALO* palo) { s_paloFeather = palo; }

void UpdateSkillFx()
{
    if (!FStep()) return;
    const float t = g_clock.t, dt = std::clamp(g_clock.dt, 0.0f, 0.1f);
    LO* pcur = PloPoCurrent(g_psw);
    // the continuous moves (the current player's only)
    {
        // (the jet drawn red while he hovers: NoteSkillFxPlaced keeps it as placed)
        std::lock_guard<std::mutex> lock(s_mtxHover);
        s_paloHoverRed = s_paloHoverNoted && s_paloHoverNoted == pcur ? s_paloHoverNoted : nullptr;
        if (!s_paloHoverRed) s_aparthover.clear();
        s_paloHoverNoted = nullptr;
        // (SLY2N_SKILLFXPRINT: how many of Bentley's groups were taken for the jet, when that changes)
        static size_t s_cPrinted = size_t(-1);
        size_t c = 0;
        for (const GHOSTPART& part : s_aparthover) c += size_t(std::count(part.afVisible.begin(), part.afVisible.end(), uint8_t(1)));
        if (std::getenv("SLY2N_SKILLFXPRINT") && c != s_cPrinted) std::printf("skillfx: hover jet groups drawn red: %zu\n", s_cPrinted = c);
    }
    if (s_paloRocket && s_paloRocket == pcur)
    {
        const glm::vec3 pos = PosFeet(s_paloRocket) + glm::vec3(0, 0, 10);
        const glm::vec3 v = dt > 0.0f && glm::length(s_posRocketLast) > 0.0f ? (pos - s_posRocketLast) / dt : glm::vec3(0.0f);
        s_posRocketLast = pos;
        for (int i = 0; i < 2; ++i)
        {
            FXP p = Dot(pos + glm::vec3(Rand(-12.f, 12.f), Rand(-12.f, 12.f), 0), -v * Rand(0.1f, 0.3f) + glm::vec3(Rand(-120.f, 120.f), Rand(-120.f, 120.f), Rand(150.f, 380.f)),
                        Rand(0.2f, 0.35f), 3.0f, 1.0f, kSpark, glm::vec4(kEmber.r, kEmber.g, kEmber.b, 0), 1600.0f, 1.0f);
            p.fStreak = true;
            Add(p);
        }
        // flames trailing from the feet, back along his way and a little up
        const glm::vec2 vh(v);
        const glm::vec3 back = glm::length(vh) > 30.0f ? glm::vec3(-glm::normalize(vh), 0.35f) : glm::vec3(0, 0, 1);
        for (int i = 0; i < 2; ++i)
            Flame(pos + glm::vec3(Rand(-10.f, 10.f), Rand(-10.f, 10.f), 0), glm::vec3(v.x, v.y, 0.0f) * 0.6f, back, Rand(0.12f, 0.2f), Rand(30.f, 48.f), Rand(7.f, 9.f));
    }
    else s_posRocketLast = glm::vec3(0.0f);
    if (s_paloFeather && s_paloFeather == pcur)
    {
        const glm::vec3 pos = PosFeet(s_paloFeather);
        if (t - s_tFeatherPuff > 0.22f && glm::length(glm::vec2(pos - s_posFeatherLast)) > 25.0f)
        {
            s_tFeatherPuff = t;
            s_posFeatherLast = pos;
            for (int i = 0; i < 4; ++i)
                Add(Dot(pos + glm::vec3(Rand(-25.f, 25.f), Rand(-25.f, 25.f), 6), glm::vec3(Rand(-70.f, 70.f), Rand(-70.f, 70.f), Rand(30.f, 90.f)),
                        Rand(0.5f, 0.8f), 14.0f, 30.0f, glm::vec4(kDust.r, kDust.g, kDust.b, 50), glm::vec4(kDust.r, kDust.g, kDust.b, 0), 0.0f, 2.5f));
        }
    }
    if (s_paloGhost && s_paloGhost == pcur && t - s_tGhostNote < 0.1f && t - s_tAdrenStreak > 0.05f)
    {
        // a few faint green streaks of speed round him
        s_tAdrenStreak = t;
        const glm::vec3 pos = PosAloCenter(s_paloGhost) + glm::vec3(Rand(-35.f, 35.f), Rand(-35.f, 35.f), Rand(20.f, 110.f));
        FXP p = Dot(pos, glm::vec3(0, 0, Rand(-40.f, 40.f)), 0.2f, 2.0f, 1.0f, glm::vec4(60, 128, 70, 70), glm::vec4(60, 128, 70, 0));
        Add(p);
    }
    s_paloRocket = s_paloFeather = nullptr;
    // Shadow Cloak: dark violet smoke curling up off him while cloaked; a ring drawn in to his feet as it closes
    {
        static float s_uPrev = 0.0f, s_tWisp = -10.0f;
        ALO* paloCloak = nullptr;
        const float uCloak = UShadowCloak(&paloCloak);
        if (paloCloak && paloCloak == pcur)
        {
            const glm::vec3 posFeet = PosFeet(paloCloak);
            if (uCloak >= 0.6f && s_uPrev < 0.6f)
            {
                Ring(posFeet + glm::vec3(0, 0, 6), 160.0f, 25.0f, 0.35f, 12.0f, glm::vec4(30, 12, 60, 110));
                for (int i = 0; i < 10; ++i)
                {
                    const float a = Rand(0.0f, 6.2831853f);
                    Add(Dot(posFeet + glm::vec3(std::cos(a) * 40.0f, std::sin(a) * 40.0f, Rand(10.f, 90.f)),
                            glm::vec3(std::cos(a) * 90.0f, std::sin(a) * 90.0f, Rand(40.f, 120.f)), Rand(0.6f, 0.9f), 18.0f, 40.0f,
                            glm::vec4(18, 8, 36, 90), glm::vec4(18, 8, 36, 0), 0.0f, 2.0f));
                }
            }
            if (uCloak > 0.3f && t - s_tWisp > 0.06f)
            {
                s_tWisp = t;
                const float a = Rand(0.0f, 6.2831853f), r = Rand(15.f, 45.f);
                Add(Dot(posFeet + glm::vec3(std::cos(a) * r, std::sin(a) * r, Rand(10.f, 120.f)),
                        glm::vec3(Rand(-25.f, 25.f), Rand(-25.f, 25.f), Rand(50.f, 110.f)), Rand(0.7f, 1.1f), 10.0f, 30.0f,
                        glm::vec4(22, 10, 44, 80.0f * uCloak), glm::vec4(40, 20, 70, 0), 0.0f, 0.8f));
            }
        }
        s_uPrev = paloCloak == pcur ? uCloak : 0.0f;
    }
    // Coin Magnet's motes: each flies to him faster as it goes, a trail behind it; gone on reaching him
    for (SKILLMOTE& m : s_amote)
    {
        if (t < m.t0 || !m.paloTo) continue;
        const glm::vec3 to = PosAloCenter(m.paloTo) + glm::vec3(0, 0, 40) - m.pos;
        const float l = glm::length(to), v = 900.0f + 2600.0f * std::min(1.0f, (t - m.t0) / 0.3f);
        if (l < v * dt + 10.0f) { m.paloTo = nullptr; continue; }
        m.pos += to / l * (v * dt);
        Add(Dot(m.pos, glm::vec3(0.0f), 0.16f, 5.0f, 1.0f, m.rgba, glm::vec4(glm::vec3(m.rgba), 0)));
    }
    s_amote.erase(std::remove_if(s_amote.begin(), s_amote.end(), [&](const SKILLMOTE& m) { return !m.paloTo || t - m.t0 > 1.5f; }), s_amote.end());
    // the homing darts' trails
    for (ALO* pdart : HomingDartsInFlight())
        Add(Dot(PosAloCenter(pdart), glm::vec3(0.0f), 0.22f, 6.0f, 1.5f, kOrange, glm::vec4(kEmber.r, kEmber.g, kEmber.b, 0)));
    // the particles move
    for (FXP& p : s_ap)
    {
        p.vel *= std::max(0.0f, 1.0f - p.drag * dt);
        p.vel.z -= p.grav * dt;
        p.pos += p.vel * dt;
    }
    s_ap.erase(std::remove_if(s_ap.begin(), s_ap.end(), [&](const FXP& p) { return t - p.t0 > p.life || t < p.t0; }), s_ap.end());
    s_aring.erase(std::remove_if(s_aring.begin(), s_aring.end(), [&](const FXRING& r) { return t - r.t0 > r.life || t < r.t0 - 1.0f; }), s_aring.end());
}

static std::vector<GHOSTPART> s_apartCloak;   // (Shadow Cloak: his pose as placed this frame)
static float s_uCloak = 0.0f;

void UpdateSkillFxGhosts(const SCENE& scene)
{
    if (!g_psw) return;
    const float t = g_clock.t;
    {
        ALO* paloCloak = nullptr;
        s_uCloak = UShadowCloak(&paloCloak);
        s_apartCloak.clear();
        if (s_uCloak > 0.0f && paloCloak && paloCloak == PloPoCurrent(g_psw))
        {
            std::vector<const SCENEGLOBSET*> apsgs;
            CollectSgs(scene, paloCloak, 0, apsgs);
            for (const SCENEGLOBSET* psgs : apsgs)
            {
                if (!psgs->pglobset) continue;
                GHOSTPART part;
                part.psgs = psgs;
                part.pglobset = psgs->pglobset;
                part.amatGlob = psgs->amatGlob;
                part.afVisible = psgs->afVisible;
                part.aamatBone = psgs->aamatBone;
                s_apartCloak.push_back(std::move(part));
            }
        }
    }
    if (s_serial != g_psw->serial) s_aghost.clear();
    while (!s_aghost.empty() && (t - s_aghost.front().t > kTGhostLife || s_aghost.front().t > t)) s_aghost.pop_front();
    if (!s_paloGhost || s_paloGhost != PloPoCurrent(g_psw) || t - s_tGhostNote > 0.1f || t - s_tGhostLast < kDtGhost) return;
    std::vector<const SCENEGLOBSET*> apsgs;
    CollectSgs(scene, s_paloGhost, 0, apsgs);
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
    while (s_aghost.size() > 6) s_aghost.pop_front();
    s_tGhostLast = t;
}

void DrawSkillFxGhosts(const SCENE& scene, const CAMERA& cam)
{
    {
        std::vector<GHOSTPART> apart;
        {
            std::lock_guard<std::mutex> lock(s_mtxHover);
            if (s_paloHoverRed) apart = s_aparthover;
        }
        if (!apart.empty())
        {
            const float u = 0.9f + 0.1f * std::sin(g_clock.tReal * 21.0f);   // (a flicker)
            std::vector<GHOSTDRAW> adraw;
            adraw.push_back({&apart, glm::vec4(kRgbHover * u, 1.0f), 1, true});
            RenderGhostPoses(scene, cam, adraw);
        }
    }
    if (!s_apartCloak.empty() && s_uCloak > 0.0f)
    {
        // Shadow Cloak: he is drawn over as a dark, violet shadow of himself, breathing a little
        const float u = s_uCloak * (0.7f + 0.08f * std::sin(g_clock.tReal * 2.5f));
        std::vector<GHOSTDRAW> adrawCloak;
        GHOSTDRAW gd;
        gd.papart = &s_apartCloak;
        gd.rgba = glm::vec4(0.07f, 0.03f, 0.14f, u);
        gd.fOver = true;
        adrawCloak.push_back(gd);
        RenderGhostPoses(scene, cam, adrawCloak);
    }
    if (s_aghost.empty() || !g_psw || s_serial != g_psw->serial) return;
    const float t = g_clock.t;
    std::vector<GHOSTDRAW> adraw;
    for (const GHOST& g : s_aghost)
    {
        const float age = t - g.t;
        if (age < kTGhostMin || age > kTGhostLife) continue;
        const float u = 1.0f - (age - kTGhostMin) / (kTGhostLife - kTGhostMin);
        adraw.push_back({&g.apart, glm::vec4(0.08f, 0.26f, 0.1f, 0.45f * u)});
    }
    RenderGhostPoses(scene, cam, adraw);
}

void DrawSkillFx(int width, int height)
{
    if (!g_psw || s_serial != g_psw->serial) return;
    const float t = g_clock.t;
    const bool fFlash = t - s_tFlash < 0.6f && t >= s_tFlash;
    ALO* paloCloak = nullptr;
    const float uCloak = UShadowCloak(&paloCloak);
    const bool fVignette = uCloak > 0.0f && paloCloak && paloCloak == PloPoCurrent(g_psw);
    if (s_ap.empty() && s_aring.empty() && !fFlash && !fVignette) return;
    constexpr float z = 0.9993f;
    g_hud.Begin();
    if (fVignette)
    {
        // Shadow Cloak: the picture's edges darkened, violet, in layers stepping in. The screen's edges in the canvas
        // (wider than 640 x 448 on a widescreen frame)
        const float xL = ((-1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f;
        const float xR = ((1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f;
        const float yT = (1.0f - (1.0f - g_display.uiOffset.y) / g_display.uiScale.y) * 224.0f;
        const float yB = (1.0f - (-1.0f - g_display.uiOffset.y) / g_display.uiScale.y) * 224.0f;
        for (int i = 0; i < 6; ++i)
        {
            const float d = 14.0f * float(i + 1);
            const glm::vec4 c(10, 4, 22, 16.0f * uCloak);
            g_hud.FillScreenRect(xL, yT, xR, yT + d, c, z - 2e-4f);
            g_hud.FillScreenRect(xL, yB - d, xR, yB, c, z - 2e-4f);
            g_hud.FillScreenRect(xL, yT + d, xL + d * 1.4f, yB - d, c, z - 2e-4f);
            g_hud.FillScreenRect(xR - d * 1.4f, yT + d, xR, yB - d, c, z - 2e-4f);
        }
    }
    for (const FXRING& r : s_aring)
    {
        const float u = (t - r.t0) / r.life;
        if (u < 0.0f || u > 1.0f) continue;
        const float e = 1.0f - (1.0f - u) * (1.0f - u);   // (fast out, slowing)
        const float rad = r.r0 + (r.r1 - r.r0) * e;
        const glm::vec3 c = r.pos + glm::vec3(0, 0, r.dz * u);
        const glm::vec4 rgba(r.rgba.r, r.rgba.g, r.rgba.b, r.rgba.a * (1.0f - u) * std::min(1.0f, u * 8.0f));
        constexpr int cseg = 40;
        glm::vec2 cPrev;
        bool fPrev = false;
        for (int i = 0; i <= cseg; ++i)
        {
            const float a = 6.2831853f * float(i) / float(cseg);
            const glm::vec3 p = c + glm::vec3(std::cos(a) * rad, std::sin(a) * rad, 0.0f);
            glm::vec2 cc;
            const bool f = FCanvas(p, &cc);
            if (f && fPrev)
            {
                const float w = std::clamp(PixelsOf(p, r.w * (1.0f - 0.5f * u), cc), 0.6f, 9.0f);
                g_hud.FillScreenLine(cPrev, cc, w, rgba, z);
            }
            cPrev = cc;
            fPrev = f;
        }
    }
    for (const FXP& p : s_ap)
    {
        const float u = std::clamp((t - p.t0) / p.life, 0.0f, 1.0f);
        const glm::vec4 rgba = glm::mix(p.rgba0, p.rgba1, u);
        glm::vec2 c;
        if (!FCanvas(p.pos, &c)) continue;
        if (p.fFlame)
        {
            // three layers (red-orange edge, orange-yellow, a pale core), each a tongue: its tip along dir, flickering
            // across, its sides curving in, a round base; thinner and redder as it ages
            const float alpha = std::pow(1.0f - u, 1.6f) * std::min(1.0f, u * 10.0f);
            const float lenk = 0.85f + 0.15f * std::sin(t * 23.0f + p.seed * 7.0f);
            const float flick = std::sin(t * 37.0f + p.seed * 13.0f), f2 = 0.25f * std::sin(t * 29.0f + p.seed * 5.0f);
            glm::vec2 cTip;
            if (!FCanvas(p.pos + p.dir * p.size0 * lenk, &cTip)) continue;
            glm::vec2 ax = cTip - c;
            const float l = glm::length(ax);
            if (l < 0.5f) ax = glm::vec2(0.0f, -1.0f);
            const glm::vec2 a = l < 0.5f ? ax : ax / l, n(-a.y, a.x);
            const float wpx = std::clamp(PixelsOf(p.pos, p.size1, c), 0.8f, 40.0f) * (1.0f - 0.45f * u);
            struct LAYER { float s, sl; glm::vec4 rgba; };
            const LAYER alayer[3] = {{1.0f, 1.0f, glm::vec4(128, 40, 8, 0.55f)}, {0.68f, 0.8f, glm::vec4(128, 92, 20, 0.75f)},
                                     {0.38f, 0.55f, glm::vec4(128, 124, 88, 0.9f)}};
            for (int il = 0; il < 3; ++il)
            {
                const LAYER& ly = alayer[il];
                if (il == 2 && u > 0.6f) break;   // (the core gone as it dies down)
                const float w = wpx * ly.s;
                const glm::vec2 tip = c + ax * ly.sl + n * flick * w * 0.5f;
                const glm::vec2 mid = c + ax * ly.sl * 0.45f + n * flick * w * 0.2f;
                glm::vec2 apt[13];
                int cpt = 0;
                apt[cpt++] = tip;
                apt[cpt++] = mid + n * w * (0.62f + f2);
                for (int k = 0; k <= 8; ++k)
                {
                    const float ang = 3.14159265f * float(k) / 8.0f;   // (from +n round the back to -n)
                    apt[cpt++] = c + (n * std::cos(ang) - a * std::sin(ang)) * w;
                }
                apt[cpt++] = mid - n * w * (0.62f - f2);
                glm::vec2 atri[13 * 3];
                int ctri = 0;
                for (int i = 0; i < cpt; ++i)
                {
                    atri[ctri++] = c;
                    atri[ctri++] = apt[i];
                    atri[ctri++] = apt[(i + 1) % cpt];
                }
                g_hud.FillScreenTris(atri, ctri, glm::vec4(glm::vec3(ly.rgba), 128.0f * ly.rgba.a * alpha), z + 1e-5f * float(il));
            }
        }
        else if (p.fStreak)
        {
            glm::vec2 c2;
            if (!FCanvas(p.pos - p.vel * 0.025f, &c2)) continue;
            g_hud.FillScreenLine(c2, c, std::clamp(PixelsOf(p.pos, p.size0 + (p.size1 - p.size0) * u, c), 0.6f, 3.0f), rgba, z + 1e-4f);
        }
        else
        {
            // a soft puff: discs one inside the other, so its edge fades out and its middle is denser
            const float rpx = std::clamp(PixelsOf(p.pos, p.size0 + (p.size1 - p.size0) * u, c), 0.5f, 40.0f);
            if (rpx < 2.5f) { g_hud.DrawRing(c.x, c.y, rpx, 0.0f, 1.0f, rgba, z); continue; }
            for (float k : {1.0f, 0.72f, 0.45f})
                g_hud.DrawRing(c.x, c.y, rpx * k, 0.0f, 1.0f, glm::vec4(glm::vec3(rgba), rgba.a * 0.4f), z);
        }
    }
    if (fFlash)
    {
        const float u = (t - s_tFlash) / 0.6f;
        g_hud.Fade(glm::vec4(128, 128, 128, 128.0f * 0.85f * s_aFlash * (1.0f - u) * (1.0f - u)));
    }
    g_hud.Flush(width, height);
}
