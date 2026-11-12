// See ancestors.h.
#include "ancestors.h"

#include "actor.h"
#include "alo.h"
#include "brx.h"
#include "clock.h"
#include "cm.h"
#include "display.h"
#include "gadget.h"
#include "hud.h"
#include "joy.h"
#include "lo.h"
#include "pcprogress.h"
#include "pickup.h"
#include "po.h"
#include "raccoonus.h"
#include "render.h"
#include "scene.h"
#include "skillfx.h"
#include "so.h"
#include "sw.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
constexpr uint32_t kBtnR1 = 0x8;
constexpr float kRSense = 2400.0f;     // Treasure Sense's reach
constexpr float kRMagnet = 420.0f;     // Coin Magnet's
constexpr float kTCloakIn = 0.4f;      // Shadow Cloak: R1 held still this long
constexpr float kVStill = 25.0f;       // and slower than this over the ground

bool FPrint() { static const bool s_f = std::getenv("SLY2N_ANCESTORPRINT") != nullptr; return s_f; }

// the main Sly as the current player (not a PC Extras guest's moves)
SLY* PslyPlayer(LO* plo)
{
    if (!plo || !g_psw || plo != PloPoCurrent(g_psw)) return nullptr;
    return PslyFromLo(plo);
}
uint32_t GrfbtnOf(LO* plo)
{
    PODATA* ppd = plo ? PpodataFromLo(plo) : nullptr;
    return ppd && ppd->ppadctl ? ppd->ppadctl->grfbtn : 0u;
}

// Shadow Cloak's state
struct CLOAK
{
    ALO* palo = nullptr;
    uint32_t serial = 0;
    float tHeld = 0.0f;     // R1 held still for
    float u = 0.0f;         // its look, 0 .. 1
    glm::vec3 posLast{0.0f};
    float tPos = -1.0f;
};
CLOAK s_cloak;

bool FCanvas(const glm::vec3& pos, glm::vec2* pc)
{
    const glm::vec3 sc = ConvertCmWorldToScreen(&g_cm, pos);
    if (sc.z <= 1.0f) return false;
    *pc = glm::vec2(((sc.x - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f,
                    (1.0f - (sc.y - g_display.uiOffset.y) / g_display.uiScale.y) * 224.0f);
    return true;
}
}  // namespace

void ApplyKnightsLeap(ACTOR* pa)
{
    if (!pa || !pa->palo || !PslyPlayer(pa->palo) || !FPowerupOwned(kItemKnightsLeap) || !(GrfbtnOf(pa->palo) & kBtnR1)) return;
    // (the double jump's launch, to about twice its height: the speed up x 1.45; along a little further)
    glm::vec3 v = VelAloPos(pa->palo);
    v.z = std::max(v.z, 300.0f) * 1.45f;
    v.x *= 1.15f;
    v.y *= 1.15f;
    SetAloVelocityTo(pa->palo, v);
    SkillFxKnightsLeap(pa->palo);
    NoteRaccoonusProgress(kItemKnightsLeap, 1.0f);
    if (FPrint()) std::printf("ancestors: Knight's Leap at %.2f (vz %.0f)\n", g_clock.t, v.z);
}

bool FShadowCloaked(const LO* pso)
{
    return pso && s_cloak.palo == pso && g_psw && s_cloak.serial == g_psw->serial && s_cloak.u >= 0.6f;
}

float UShadowCloak(ALO** ppalo)
{
    if (ppalo) *ppalo = s_cloak.palo;
    return g_psw && s_cloak.serial == g_psw->serial ? s_cloak.u : 0.0f;
}

bool FCoinMagnetTakes(LO* ppickup, LO* pplayer)
{
    if (!ppickup || !PslyPlayer(pplayer) || !FPowerupOwned(kItemCoinMagnet)) return false;
    if (!FIsLoDerivedFrom(ppickup, 0x5B) && !FIsLoDerivedFrom(ppickup, 0x5F)) return false;   // (coins, clue bottles)
    const glm::vec3 pos = PosAloCenter(static_cast<ALO*>(ppickup));
    if (glm::length(pos - PosAloCenter(static_cast<ALO*>(pplayer))) > kRMagnet) return false;
    SkillFxMagnet(pos, static_cast<ALO*>(pplayer), FIsLoDerivedFrom(ppickup, 0x5F));
    NoteRaccoonusProgress(kItemCoinMagnet, 1.0f);
    return true;
}

void UpdateAncestorTechniques()
{
    if (!g_psw) return;
    if (s_cloak.serial != g_psw->serial) s_cloak = CLOAK{}, s_cloak.serial = g_psw->serial;
    const float t = g_clock.t, dt = std::clamp(g_clock.dt, 0.0f, 0.1f);
    LO* plo = PloPoCurrent(g_psw);
    SLY* psly = PslyPlayer(plo);
    bool fOn = false;
    if (psly && FPowerupOwned(kItemShadowCloak))
    {
        // (his speed over the ground from his place a step ago: he moves by the step physics, VelAloPos is zero)
        const glm::vec3 pos = PosAloCenter(psly->palo);
        const float dtPos = t - s_cloak.tPos;
        const float v = s_cloak.palo == psly->palo && dtPos > 0.0f && dtPos < 0.25f ? glm::length(glm::vec2(pos - s_cloak.posLast)) / dtPos : 0.0f;
        s_cloak.posLast = pos;
        s_cloak.tPos = t;
        s_cloak.palo = psly->palo;
        const bool fStill = v < kVStill && (psly->state == 0 || psly->state == 1) && psly->pxpFloor;
        s_cloak.tHeld = fStill && (GrfbtnOf(plo) & kBtnR1) ? s_cloak.tHeld + dt : 0.0f;
        fOn = s_cloak.tHeld >= kTCloakIn;
    }
    else s_cloak.tHeld = 0.0f;
    const float uOld = s_cloak.u;
    s_cloak.u = std::clamp(s_cloak.u + (fOn ? dt / 0.35f : -dt / 0.2f), 0.0f, 1.0f);
    if (FPrint() && (uOld >= 0.6f) != (s_cloak.u >= 0.6f)) std::printf("ancestors: Shadow Cloak %s at %.2f\n", s_cloak.u >= 0.6f ? "on" : "off", t);
}

// Treasure Sense: the clue bottles still to find within kRSense of Sly, their own models with a glowing outer edge
// seen through walls (render.h RenderXrayOutlines), brighter nearer, each with a slow pulse of its own. The bottles
// are the streaming pool's instances (class 0x5F) sharing one model: their placements in the scene are those globsets
// placed at a bottle still to find (pickup.cpp AposCluesLeft).
void DrawTreasureSense(const SCENE& scene, const CAMERA& cam)
{
    if (!g_psw) return;
    LO* plo = PloPoCurrent(g_psw);
    SLY* psly = PslyPlayer(plo);
    if (!psly || !FPowerupOwned(kItemTreasureSense)) return;
    static std::vector<glm::vec3> s_apos;
    s_apos.clear();
    AposCluesLeft(s_apos);
    if (s_apos.empty()) return;
    const glm::vec3 posSly = PosAloCenter(psly->palo);
    std::vector<const GLOBSET*> apgs;
    for (LO* pinst : g_psw->mpcidaplo[0x5F])
        if (pinst && pinst->fInWorld && !pinst->fRemoved && FIsLoDerivedFrom(pinst, 0x30))
            if (const GLOBSET* pgs = static_cast<ALO*>(pinst)->pglobset.get(); pgs && std::find(apgs.begin(), apgs.end(), pgs) == apgs.end())
                apgs.push_back(pgs);
    if (apgs.empty()) return;
    static std::vector<XRAYPART> s_apart;
    s_apart.clear();
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.pglobset || sgs.irtgt >= 0 || std::find(apgs.begin(), apgs.end(), sgs.pglobset.get()) == apgs.end()) continue;
        const glm::vec3 pos = sgs.fBounds ? sgs.posBounds : sgs.amatGlob.empty() ? glm::vec3(1e9f) : glm::vec3(sgs.amatGlob[0][3]);
        bool fLeft = false;
        for (const glm::vec3& p : s_apos) fLeft = fLeft || glm::length(p - pos) < 250.0f;
        if (!fLeft) continue;
        const float d = glm::length(pos - posSly);
        if (d > kRSense) continue;
        const float near = 1.0f - d / kRSense;
        const float pulse = 0.8f + 0.2f * std::sin(g_clock.tReal * 3.0f + pos.x * 0.01f + pos.y * 0.013f);
        s_apart.push_back({&sgs, (0.35f + 0.65f * near) * pulse});
    }
    if (s_apart.empty()) return;
    RenderXrayOutlines(scene, cam, s_apart, glm::vec3(0.35f, 1.0f, 0.85f));
}
