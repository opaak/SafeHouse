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
#include "pcprogress.h"
#include "po.h"
#include "sound.h"
#include "sw.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
constexpr uint32_t GRF_CIRCLE = 0x20;   // (the PADCTL's layout, as interact.cpp's)
constexpr float kAcrossMin = 250.0f, kAcrossMax = 3200.0f;   // (Sly's own spire jumps reach about 600)
constexpr float kUpMax = 900.0f, kDownMax = 2000.0f;
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
};
AIM s_aim;

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
    return true;
}

void DrawLeapDragonHud(int width, int height)
{
    if (!s_aim.fAiming || g_clock.tReal - s_aim.tLast > 0.1f) return;
    if (!g_psw || s_aim.serial != g_psw->serial) return;
    LO* ptarget = s_aim.ploTarget;
    if (!ptarget || !ptarget->fInWorld) return;
    const glm::vec3 sc = ConvertCmWorldToScreen(&g_cm, PosJtLo(ptarget));   // 0x14F308: (x, y) NDC, depth
    if (sc.z <= 1.0f || std::fabs(sc.x) > 1.2f || std::fabs(sc.y) > 1.2f) return;
    // the picture's NDC on the 2D canvas (as pccharacter.cpp's crosshair)
    const glm::vec2 c(((sc.x - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f,
                      (1.0f - (sc.y - g_display.uiOffset.y) / g_display.uiScale.y) * 224.0f);
    // a ring that breathes (GS colours, 0x80 = 1): Rioichi's mahogany over a dark outline, a white dot in it
    const float u = 0.5f + 0.5f * std::sin(g_clock.tReal * 6.0f);
    const float r = 15.0f + 3.0f * u, w = 3.0f;
    const glm::vec4 rgba(128, 52, 28, 128), rgbaDark(0, 0, 0, 80), rgbaDot(128, 128, 128, 128);
    constexpr float z = 0.9995f;
    g_hud.Begin();
    g_hud.DrawRing(c.x, c.y, r + 1.5f, r - w - 1.5f, 1.0f, rgbaDark, z - 1e-4f);
    g_hud.DrawRing(c.x, c.y, r, r - w, 1.0f, rgba, z);
    // four short chevrons pointing in at it
    const float kx = 448.0f / 480.0f;
    const float g0 = r + 4.0f, g1 = r + 11.0f;
    const glm::vec2 adir[4] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    for (glm::vec2 d : adir)
    {
        d = glm::normalize(d);
        const glm::vec2 a = c + glm::vec2(d.x * g0 * kx, d.y * g0), b = c + glm::vec2(d.x * g1 * kx, d.y * g1);
        g_hud.FillScreenLine(a, b, w + 2.0f, rgbaDark, z - 1e-4f);
        g_hud.FillScreenLine(a, b, w, rgba, z);
    }
    g_hud.FillScreenRect(c.x - 1.5f, c.y - 1.5f, c.x + 1.5f, c.y + 1.5f, rgbaDot, z);
    g_hud.Flush(width, height);
}
