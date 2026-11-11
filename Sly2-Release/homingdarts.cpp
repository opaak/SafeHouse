// (PC) Sly 4's Homing Darts for Bentley: the lock-on, the steering and the lock's brackets (homingdarts.h).
#include "homingdarts.h"

#include "activate.h"
#include "actor.h"
#include "brx.h"
#include "gadget.h"
#include "alo.h"
#include "clock.h"
#include "cm.h"
#include "display.h"
#include "gang.h"
#include "guard.h"
#include "hud.h"
#include "lo.h"
#include "pcprogress.h"
#include "po.h"
#include "so.h"
#include "sw.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
constexpr float kRScreen = 0.35f;      // the lock's reach from the crosshair (of the picture's half, NDC)
constexpr float kDistMax = 6000.0f;    // and in the world
constexpr float kRadTurn = 9.0f;       // a dart's turn, radians a second
constexpr float kTLockIn = 0.22f;      // the brackets' closing in on a new lock, seconds

struct LOCK
{
    LO* plo = nullptr;
    uint32_t serial = 0;
    float tReal = -1.0f;       // the last step it was kept
    float tRealLocked = 0.0f;  // when it was taken
    float tRealFired = -1.0f;  // the last dart at it
} s_lock;

struct DART { ALO* palo = nullptr; LO* ptarget = nullptr; uint32_t serial = 0; float tReal = 0; };
std::vector<DART> s_adart;

bool FLive(LO* plo) { return plo && plo->fInWorld && g_psw; }

glm::vec3 PosAim(LO* plo) { return PosAloCenter(static_cast<ALO*>(plo)); }

// in sight from the camera's near point: nothing solid but the guard itself and Bentley before it
bool FInSight(const glm::vec3& a, LO* ptarget, ALO* paloBentley)
{
    const glm::vec3 b = PosAim(ptarget);
    std::vector<SOE*> apsoe;
    for (SOE* pe : AsolidWithTree(a, b))
        if (pe->palo != paloBentley && pe->palo != ptarget && PloRoot(pe->palo) != PloRoot(ptarget)) apsoe.push_back(pe);
    LSG lsg;
    if (!PsoeHitTestLineObjects(1, a, b, apsoe, &lsg)) return true;
    return glm::distance(lsg.pos0, a) > glm::distance(a, b) - 60.0f;
}

bool FCanvasOf(const glm::vec3& pos, glm::vec2* pc)
{
    const glm::vec3 sc = ConvertCmWorldToScreen(&g_cm, pos);
    if (sc.z <= 1.0f) return false;
    *pc = glm::vec2(((sc.x - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f,
                    (1.0f - (sc.y - g_display.uiOffset.y) / g_display.uiScale.y) * 224.0f);
    return true;
}

bool FLockShown()
{
    return FLive(s_lock.plo) && s_lock.serial == g_psw->serial && g_clock.tReal - s_lock.tReal <= 0.1f;
}
}  // namespace

void UpdateHomingLock(BENTLEY* p)
{
    if (!g_psw || !FPowerupOwned(kItemHomingDarts)) { s_lock.plo = nullptr; return; }
    ALO* palo = p->palo;
    if (PloPoCurrent(g_psw) != palo) { s_lock.plo = nullptr; return; }
    const glm::vec3 a = ConvertCmScreenToWorld(&g_cm, glm::vec3(0.0f, 0.0f, 10.0f));
    LO* pbest = nullptr;
    float best = kRScreen * kRScreen;
    for (const auto& up : g_psw->aploAll)
    {
        LO* plo = up.get();
        if (!plo->fInWorld || !FCidDerivedFrom(plo->Cid(), 0x38) || !FIsLoDerivedFrom(plo, 0x30) || !PguardFromLo(plo)) continue;
        const glm::vec3 pos = PosAim(plo);
        if (glm::distance(pos, a) > kDistMax) continue;
        const glm::vec3 sc = ConvertCmWorldToScreen(&g_cm, pos);
        if (sc.z <= 1.0f) continue;
        float d2 = sc.x * sc.x + sc.y * sc.y;
        if (plo == s_lock.plo) d2 *= 0.6f;   // (the lock held a little: no flicker between two guards side by side)
        if (d2 < best && FInSight(a, plo, palo)) { best = d2; pbest = plo; }
    }
    if (pbest != s_lock.plo || s_lock.serial != g_psw->serial) s_lock.tRealLocked = g_clock.tReal, s_lock.tRealFired = -1.0f;
    s_lock.plo = pbest;
    s_lock.serial = g_psw->serial;
    s_lock.tReal = g_clock.tReal;
}

std::vector<ALO*> HomingDartsInFlight()
{
    std::vector<ALO*> a;
    if (!g_psw) return a;
    for (const DART& d : s_adart)
        if (d.serial == g_psw->serial && d.palo && d.palo->fInWorld && g_clock.tReal - d.tReal < 4.5f) a.push_back(d.palo);
    return a;
}

LO* PloHomingLock() { return FLockShown() ? s_lock.plo : nullptr; }

void NoteHomingDart(ALO* pdart, LO* ptarget)
{
    if (!pdart || !ptarget || !g_psw) return;
    s_adart.erase(std::remove_if(s_adart.begin(), s_adart.end(), [&](const DART& d) {
        return d.palo == pdart || d.serial != g_psw->serial || g_clock.tReal - d.tReal > 5.0f; }), s_adart.end());
    s_adart.push_back({pdart, ptarget, g_psw->serial, g_clock.tReal});
    s_lock.tRealFired = g_clock.tReal;
}

void SteerHomingDart(ALO* pdart, glm::vec3* pdir)
{
    if (s_adart.empty() || !g_psw) return;
    auto it = std::find_if(s_adart.begin(), s_adart.end(), [&](const DART& d) { return d.palo == pdart; });
    if (it == s_adart.end()) return;
    if (it->serial != g_psw->serial || !FLive(it->ptarget) || !pdart->fInWorld || g_clock.tReal - it->tReal > 4.5f)
    {
        s_adart.erase(it);
        return;
    }
    const glm::vec3 to = PosAim(it->ptarget) - PosAloCenter(pdart);
    const float l = glm::length(to);
    if (l < 1.0f) return;
    const glm::vec3 want = to / l;
    const float c = std::clamp(glm::dot(*pdir, want), -1.0f, 1.0f);
    const float ang = std::acos(c);
    if (ang > 1.75f)   // (past it: flies on straight)
    {
        s_adart.erase(it);
        return;
    }
    const float step = kRadTurn * g_clock.dt;
    if (ang <= step) { *pdir = want; return; }
    // turned by step within the plane of the two (slerp)
    const float s = std::sin(ang);
    *pdir = glm::normalize((*pdir) * (std::sin(ang - step) / s) + want * (std::sin(step) / s));
}

void DrawHomingLockHud(int width, int height)
{
    if (!FLockShown()) return;
    glm::vec2 c;
    if (!FCanvasOf(PosAim(s_lock.plo), &c)) return;
    constexpr float z = 0.9995f;
    const float tIn = std::clamp((g_clock.tReal - s_lock.tRealLocked) / kTLockIn, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - tIn) * (1.0f - tIn);
    // a pulse when a dart goes
    const float tFire = s_lock.tRealFired < 0.0f ? 1.0f : std::clamp((g_clock.tReal - s_lock.tRealFired) / 0.3f, 0.0f, 1.0f);
    const float r = 34.0f - 16.0f * ease + 6.0f * (1.0f - tFire);
    const float arm = 7.0f, w = 2.5f;
    const float ang = 0.785398f * (1.0f - ease) + 0.6f * std::sin(g_clock.tReal * 1.5f) * 0.15f;
    const float aIn = 128.0f * (0.35f + 0.65f * ease);
    const glm::vec4 rgba(120, 52, 8, aIn), rgbaShadow(0, 0, 0, aIn * 0.6f);
    g_hud.Begin();
    // four corner brackets, turned in as they close
    const float ca = std::cos(ang), sa = std::sin(ang);
    auto rot = [&](glm::vec2 v) { return c + glm::vec2(v.x * ca - v.y * sa, v.x * sa + v.y * ca) ; };
    for (int i = 0; i < 4; ++i)
    {
        const glm::vec2 sgn(i & 1 ? 1.0f : -1.0f, i & 2 ? 1.0f : -1.0f);
        const glm::vec2 corner = sgn * r;
        const glm::vec2 a0 = rot(corner), a1 = rot(corner - glm::vec2(sgn.x * arm, 0.0f)), a2 = rot(corner - glm::vec2(0.0f, sgn.y * arm));
        g_hud.FillScreenLine(a0 + glm::vec2(1, 1), a1 + glm::vec2(1, 1), w + 1.0f, rgbaShadow, z - 1e-4f);
        g_hud.FillScreenLine(a0 + glm::vec2(1, 1), a2 + glm::vec2(1, 1), w + 1.0f, rgbaShadow, z - 1e-4f);
        g_hud.FillScreenLine(a0, a1, w, rgba, z);
        g_hud.FillScreenLine(a0, a2, w, rgba, z);
    }
    // the centre: a small dot once locked
    if (ease >= 1.0f) g_hud.DrawRing(c.x, c.y, 3.0f, 0.0f, 1.0f, rgba, z);
    g_hud.Flush(width, height);
}
