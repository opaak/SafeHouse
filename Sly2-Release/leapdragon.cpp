// See leapdragon.h.

#include "leapdragon.h"

#include "actor.h"
#include "clock.h"
#include "cm.h"
#include "display.h"
#include "gadget.h"
#include "hud.h"
#include "interact.h"
#include "jt.h"
#include "jump.h"
#include "lo.h"
#include "activate.h"
#include "alo.h"
#include "brx.h"
#include "pcprogress.h"
#include "render.h"
#include "scene.h"
#include "raccoonus.h"
#include "po.h"
#include "sound.h"
#include "sw.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace
{
constexpr uint32_t GRF_CIRCLE = 0x20;   // (the PADCTL's layout, as interact.cpp's)
constexpr float kAcrossMin = 250.0f, kAcrossMax = 2400.0f;   // (Sly's own spire jumps reach about 600)
constexpr float kUpMax = 900.0f, kDownMax = 1600.0f;
constexpr float kCosAim = 0.766f;       // cos 40 degrees
constexpr int kTriesMax = 4;            // the best few by aim tried for a clear path (each is a swept test)

struct AIM
{
    bool fAiming = false;
    uint32_t serial = 0;    // the world's (a new world drops the aim and its objects)
    float tLast = -1.0f;    // the last step it was updated, real time (off the spire, or paused: the aim is stale)
    LO* ploOn = nullptr;    // the spire perched on
    // circle already held when Sly got onto this spire (circle's own spire target in the air): not an aim until it is
    // let go and pressed again, or letting it go would leap at once
    LO* ploSeen = nullptr;
    float tSeen = -1.0f;
    bool fWaitRelease = false;
    LO* ploTarget = nullptr;
    LEAPPLAN plan;
    ACTOR* pa = nullptr;    // (who aims: the aura's proxy is his)
};
AIM s_aim;

// The red aura: an instance of the player's target aura (jt.h PloNewLeapAura) on the target, hidden from the scene and
// drawn red (DrawLeapDragonAura) from its pose as placed (NoteLeapAuraPlaced); faded out over tFade when the aim ends,
// then let go (its pool takes it back for the blue ones).
constexpr float tAuraFade = 0.3f;
const glm::vec3 rgbAura(1.0f, 0.12f, 0.06f);
struct AURA
{
    LO* plo = nullptr;
    uint32_t serial = 0;
    float tOff = -1.0f;              // real time the fade out began (-1: shown)
    std::vector<GHOSTPART> apart;    // its entries' pose as last placed
};
AURA s_aura;
std::mutex s_mtxAura;   // (the placement runs on the pass helpers)
// the leap in flight: its spire, until Sly is perched on it (a landed leap: raccoonus.h NoteLeapLanded) or 5 s
LO* s_ploLeapTo = nullptr;
float s_tLeap = 0.0f;

bool FPrint() { static const bool s_f = std::getenv("SLY2N_LEAPPRINT") != nullptr; return s_f; }

void DropAim()
{
    s_aim.fAiming = false;
    s_aim.ploOn = s_aim.ploTarget = nullptr;
}

// The direction to aim along (world, horizontal): the left stick's when pushed, else the camera's view.
glm::vec2 DirAim(ACTOR* pa, PADCTL* pc)
{
    if (pc && pc->uJoy > 0.3f) return glm::vec2(std::cos(pc->radJoy), std::sin(pc->radJoy));
    glm::vec2 d(g_cm.mat[0].x, g_cm.mat[0].y);
    if (glm::length(d) < 1e-3f) d = glm::vec2(std::cos(pa->radHeading), std::sin(pa->radHeading));
    return glm::normalize(d);
}

// The spire to leap to (see leapdragon.h), with its plan; null when none.
LO* PloChooseTarget(ACTOR* pa, LO* ploOn, PADCTL* pc, LEAPPLAN* pplan)
{
    SW* psw = g_psw;
    if (!psw) return nullptr;
    const glm::vec2 dir = DirAim(pa, pc);
    const glm::vec3 pos = PosJtLo(ploOn ? ploOn : static_cast<LO*>(pa->palo));
    const uint32_t grfMask = static_cast<uint32_t>(pa->n1000);
    struct CAND { LO* plo; float g; };
    std::vector<CAND> acand;
    for (LO* pspire : AploJtList(psw, JTL_Spire))
    {
        if (pspire == ploOn || !pspire->fInWorld) continue;
        if (!(JtDescOf(pspire, JTL_Spire).mask & grfMask)) continue;
        const glm::vec3 d = PosJtLo(pspire) - pos;
        const float across = glm::length(glm::vec2(d));
        if (across < kAcrossMin || across > kAcrossMax || d.z > kUpMax || d.z < -kDownMax) continue;
        const float c = glm::dot(glm::vec2(d) / across, dir);
        if (c < kCosAim) continue;
        // the angle off the aim first, then the distance (a nearer one at about the same angle wins)
        acand.push_back({pspire, (1.0f - c) * 6.0f + across / kAcrossMax});
    }
    std::sort(acand.begin(), acand.end(), [](const CAND& a, const CAND& b) { return a.g < b.g; });
    for (size_t i = 0; i < acand.size() && i < size_t(kTriesMax); ++i)
        if (FPlanLeapToSpire(pa, acand[i].plo, pplan)) return acand[i].plo;
    return nullptr;
}
}  // namespace

bool FLeapDragonSpire(ACTOR* pa, LO* ploSpire, PADCTL* pc)
{
    SW* psw = g_psw;
    if (psw && s_aim.serial != psw->serial)
    {
        s_aim = AIM{};
        s_aim.serial = psw->serial;
        s_ploLeapTo = nullptr;
    }
    if (s_ploLeapTo && (ploSpire == s_ploLeapTo || g_clock.t - s_tLeap > 5.0f))
    {
        if (ploSpire == s_ploLeapTo)
        {
            if (FPrint()) std::printf("leap: landed on spire %d\n", ploSpire->oid);
            NoteLeapLanded();
        }
        s_ploLeapTo = nullptr;
    }
    // (a stale aim, from before Sly left a spire, or another spire's)
    if (s_aim.fAiming && (g_clock.tReal - s_aim.tLast > 0.25f || s_aim.ploOn != ploSpire)) DropAim();
    const bool fHeld = pc && (pc->grfbtn & GRF_CIRCLE);
    if (ploSpire != s_aim.ploSeen || g_clock.tReal - s_aim.tSeen > 0.25f) s_aim.fWaitRelease = fHeld;   // (just got on it)
    s_aim.ploSeen = ploSpire;
    s_aim.tSeen = g_clock.tReal;
    if (s_aim.fWaitRelease && !fHeld) s_aim.fWaitRelease = false;
    const bool fAllowed = psw && pc && pa && pa->palo && PslyFromLo(pa->palo) && pa->palo == PloPoCurrent(psw) &&
                          FPowerupOwned(kItemLeapingDragon);
    if (!fAllowed)
    {
        if (s_aim.fAiming) DropAim();
        return false;
    }
    if (s_aim.fWaitRelease) return false;
    if (fHeld)
    {
        if (!s_aim.fAiming && FPrint()) std::printf("leap: aiming from spire %d\n", ploSpire ? ploSpire->oid : -1);
        s_aim.fAiming = true;
        s_aim.pa = pa;
        s_aim.ploOn = ploSpire;
        s_aim.tLast = g_clock.tReal;
        LO* plo = PloChooseTarget(pa, ploSpire, pc, &s_aim.plan);
        if (plo != s_aim.ploTarget && plo) PsndStartSound(0x35);   // (the menus' step: a new target marked)
        s_aim.ploTarget = plo;
        return false;
    }
    if (!s_aim.fAiming) return false;
    LO* ptarget = s_aim.ploTarget;
    DropAim();
    if (!ptarget || !ptarget->fInWorld) return false;
    // (planned again from where Sly is now: the spire or the target may have moved since)
    LEAPPLAN plan;
    if (!FPlanLeapToSpire(pa, ptarget, &plan)) return false;
    if (FPrint()) std::printf("leap: to spire %d in %.2f s\n", ptarget->oid, plan.t);
    FaceActorToward(pa, ptarget);   // 0x2046F0
    TurnActorNow(pa);               // 0x204CA8
    SetJumpKind(&pa->jump, 1, 0, nullptr);   // 0x195A00: the jump off the spire, as cross does
    StartLeapToSpire(&pa->jump, ptarget, plan);
    s_ploLeapTo = ptarget;
    s_tLeap = g_clock.t;
    return true;
}

namespace
{
bool FUnder(const ALO* palo, const LO* proot)
{
    for (const ALO* p = palo; p; p = p->paloParent)
        if (p == proot) return true;
    return false;
}

// the screen point (canvas pixels) of a world point; false behind the camera
bool FCanvasOf(const glm::vec3& pos, glm::vec2* pc)
{
    const glm::vec3 sc = ConvertCmWorldToScreen(&g_cm, pos);   // 0x14F308: (x, y) NDC, depth
    if (sc.z <= 1.0f) return false;
    *pc = glm::vec2(((sc.x - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f,
                    (1.0f - (sc.y - g_display.uiOffset.y) / g_display.uiScale.y) * 224.0f);
    return true;
}

bool FAimShown()
{
    return s_aim.fAiming && g_clock.tReal - s_aim.tLast <= 0.1f && g_psw && s_aim.serial == g_psw->serial && s_aim.ploTarget &&
           s_aim.ploTarget->fInWorld;
}
}  // namespace

void UpdateLeapDragon()
{
    SW* psw = g_psw;
    std::lock_guard<std::mutex> lock(s_mtxAura);
    if (!psw || s_aura.serial != (psw ? psw->serial : 0))
    {
        s_aura = AURA{};
        s_aura.serial = psw ? psw->serial : 0;
        if (!psw) return;
    }
    if (FAimShown())
    {
        const glm::vec3 pos = PosJtLo(s_aim.ploTarget);
        if (s_aura.plo && s_aura.tOff >= 0.0f)
        {
            // (fading out: shown again)
            SwitchLoFaded(s_aura.plo, true, 0.15f);
            s_aura.tOff = -1.0f;
        }
        if (!s_aura.plo && s_aim.pa)
        {
            s_aura.plo = PloNewLeapAura(s_aim.pa, pos);
            s_aura.apart.clear();
            s_aura.tOff = -1.0f;
            if (FPrint()) std::printf("leap: red aura %d\n", s_aura.plo ? s_aura.plo->oid : -1);
        }
        if (s_aura.plo && FIsLoDerivedFrom(s_aura.plo, 0x30)) SetAloPos(static_cast<ALO*>(s_aura.plo), pos);
        return;
    }
    if (!s_aura.plo) return;
    if (s_aura.tOff < 0.0f)
    {
        SwitchLoFaded(s_aura.plo, false, tAuraFade);   // 0x1F7BF8 (as a dropped target's)
        s_aura.tOff = g_clock.tReal;
    }
    else if (g_clock.tReal - s_aura.tOff > tAuraFade + 0.1f)
    {
        s_aura.plo = nullptr;   // (faded out: the pool's again)
        s_aura.apart.clear();
    }
}

void NoteLeapAuraPlaced(ALO* palo, SCENEGLOBSET& sgs)
{
    if (!s_aura.plo || !palo) return;   // (the common case, without the lock)
    std::lock_guard<std::mutex> lock(s_mtxAura);
    if (!s_aura.plo || !FUnder(palo, s_aura.plo) || !sgs.pglobset) return;
    GHOSTPART* ppart = nullptr;
    for (GHOSTPART& part : s_aura.apart)
        if (part.psgs == &sgs) ppart = &part;
    if (!ppart) { s_aura.apart.emplace_back(); ppart = &s_aura.apart.back(); }
    ppart->psgs = &sgs;
    ppart->pglobset = sgs.pglobset;
    ppart->amatGlob = sgs.amatGlob;
    ppart->afVisible = sgs.afVisible;
    ppart->aamatBone = sgs.aamatBone;
    std::fill(sgs.afVisible.begin(), sgs.afVisible.end(), 0);   // (not the scene's: its blue)
}

void DrawLeapDragonAura(const SCENE& scene, const CAMERA& cam)
{
    std::vector<GHOSTPART> apart;
    float a = 0.0f;
    {
        std::lock_guard<std::mutex> lock(s_mtxAura);
        if (!s_aura.plo || s_aura.apart.empty()) return;
        apart = s_aura.apart;
        a = FIsLoDerivedFrom(s_aura.plo, 0x30) ? std::clamp(static_cast<ALO*>(s_aura.plo)->gFade, 0.0f, 1.0f) : 1.0f;
    }
    if (a <= 0.0f) return;
    // a slow breath, as the aim's
    const float u = 0.85f + 0.15f * std::sin(g_clock.tReal * 6.0f);
    std::vector<GHOSTDRAW> adraw;
    adraw.push_back({&apart, glm::vec4(rgbAura * u, a), 1});
    RenderGhostPoses(scene, cam, adraw);
}

void DrawLeapDragonHud(int width, int height)
{
    if (!FAimShown()) return;
    LO* ptarget = s_aim.ploTarget;
    const glm::vec3 posTarget = PosJtLo(ptarget);
    bool fAura = false;
    {
        std::lock_guard<std::mutex> lock(s_mtxAura);
        fAura = s_aura.plo && !s_aura.apart.empty() && s_aura.tOff < 0.0f;
    }
    g_hud.Begin();
    constexpr float z = 0.9995f;
    const glm::vec4 rgbaRed(128, 20, 10, 110), rgbaDark(20, 0, 0, 70);

    // the leap's arc: Sly's start to the spire (the flight's own curve: its start and end points and velocities, as the
    // driven record's), red dashes, fainter toward Sly
    if (ACTOR* pa = s_aim.pa; pa && s_aim.plan.t > 0.0f)
    {
        const LEAPPLAN& p = s_aim.plan;
        const glm::vec3 base(pa->vecBase);
        const glm::vec3 p0 = p.pos0 + base, p1 = p.posT + base;
        const glm::vec3 m0 = p.vLaunch * p.t, m1 = p.velT * p.t;
        constexpr int cSeg = 40;
        const float tPhase = std::fmod(g_clock.tReal * 2.0f, 1.0f);   // (the dashes run toward the spire)
        glm::vec2 cPrev(0.0f);
        bool fPrev = false;
        for (int i = 0; i <= cSeg; ++i)
        {
            const float s = float(i) / float(cSeg);
            const float s2 = s * s, s3 = s2 * s;
            const glm::vec3 pos = (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * m0 + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * m1;
            glm::vec2 c;
            const bool f = FCanvasOf(pos, &c);
            // every other segment, the dashes moving along
            const bool fDash = std::fmod(float(i) * 0.5f - tPhase, 1.0f) < 0.5f;
            if (f && fPrev && fDash && s > 0.06f)
            {
                const float a = 0.35f + 0.65f * s;
                g_hud.FillScreenLine(cPrev, c, 4.5f, glm::vec4(rgbaDark.r, rgbaDark.g, rgbaDark.b, rgbaDark.a * a), z - 1e-4f);
                g_hud.FillScreenLine(cPrev, c, 2.5f, glm::vec4(rgbaRed.r, rgbaRed.g, rgbaRed.b, rgbaRed.a * a), z);
            }
            cPrev = c;
            fPrev = f;
        }
    }

    // without the aura (none free, or its proxy missing): a red ring on the target instead
    glm::vec2 c;
    if (!fAura && FCanvasOf(posTarget, &c) && std::fabs(c.x - 320.0f) < 400.0f && std::fabs(c.y - 224.0f) < 280.0f)
    {
        const float u = 0.5f + 0.5f * std::sin(g_clock.tReal * 6.0f);
        const float r = 15.0f + 3.0f * u, w = 3.0f;
        g_hud.DrawRing(c.x, c.y, r + 1.5f, r - w - 1.5f, 1.0f, glm::vec4(0, 0, 0, 80), z - 1e-4f);
        g_hud.DrawRing(c.x, c.y, r, r - w, 1.0f, glm::vec4(128, 24, 12, 128), z);
    }
    g_hud.Flush(width, height);
}
