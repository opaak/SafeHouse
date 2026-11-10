// See ninjamoves.h.

#include "ninjamoves.h"

#include "actor.h"
#include "brx.h"
#include "clock.h"
#include "emitter.h"
#include "gadget.h"
#include "lo.h"
#include "pccharacter.h"
#include "pcprogress.h"
#include "po.h"
#include "render.h"
#include "so.h"
#include "sw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace
{
constexpr uint32_t BTN_R1 = 0x8;   // (the PADCTL's layout, as sly.cpp's)

constexpr float kRailSprint = 1.75f;
constexpr float kClimbCobra = 1.8f;

// The afterimages: a pose every dtGhost of sprinting, each drawn tGhostLife after it was taken, the newest skipped
// until it is tGhostMin old (it would lie on Sly himself); dark violet, alpha from aGhost fading to 0.
constexpr float dtGhost = 0.05f, tGhostLife = 0.3f, tGhostMin = 0.045f;
constexpr float aGhost = 0.42f;
const glm::vec3 rgbGhost(0.05f, 0.02f, 0.10f);
constexpr size_t cGhostMax = 8;

struct GHOST
{
    float t = 0.0f;
    std::vector<GHOSTPART> apart;
};
std::deque<GHOST> s_aghost;
uint32_t s_serialGhost = 0;
float s_tLastGhost = -1e9f;

bool FPrint() { static const bool s_f = std::getenv("SLY2N_NINJAPRINT") != nullptr; return s_f; }

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
    if (!psly || psly->state != 1 || !FPowerupOwned(kItemSilentSprint)) return;
    if (t - s_tLastGhost < dtGhost) return;
    if (glm::length(glm::vec2(VelAloPos(psly->palo))) < 150.0f) return;   // (standing in the sprint state: no trail)
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
        adraw.push_back({&g.apart, glm::vec4(rgbGhost, aGhost * u * u)});
    }
    RenderGhostPoses(scene, cam, adraw);
}
