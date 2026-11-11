// See extrapowerups.h.

#include "extrapowerups.h"

#include "actor.h"
#include "brx.h"
#include "clock.h"
#include "gadget.h"
#include "pcprogress.h"
#include "sound.h"
#include "sw.h"
#include "so.h"
#include "lo.h"
#include "guard.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace
{
bool FPrint() { static const bool s_f = std::getenv("SLY2N_GADGETPRINT") != nullptr; return s_f; }

// ---- Meter Recharger (Sly 4: Sly's, Bentley's, Murray's): the gadget meter full at once, then a while to recharge
constexpr float kTMeterRecharge = 60.0f;   // s of game time before it can be used again
std::unordered_map<int, float> s_mpidTUsed;   // the item: when it was last used (g_clock.t)

bool FMeterRechargerReady(int id)
{
    const auto it = s_mpidTUsed.find(id);
    // (the game clock starts again with a world: a time ahead of it is an earlier world's)
    return it == s_mpidTUsed.end() || g_clock.t < it->second || g_clock.t - it->second >= kTMeterRecharge;
}
// passive (UpdatePcPowerupsPassive): the meter run empty fills again, if ready: it rises over kTMeterFill (a slide up, the
// meter's flash at its start), not at once
constexpr float kTMeterFill = 0.9f;
std::unordered_map<int, float> s_mpidTFill;   // the item: when its fill began (g_clock.t); absent: not filling
void UpdateMeterRecharger(ACTOR* pa, int id)
{
    if (!FPowerupOwned(id) || pa->n1300 <= 0) return;
    if (auto it = s_mpidTFill.find(id); it != s_mpidTFill.end())
    {
        // (filling: the power up by its share of the time; done at full, or if the clock went back with a new world)
        if (g_clock.t < it->second) { s_mpidTFill.erase(it); return; }
        const int n = std::min(pa->n1300, NActorPower(pa) + std::max(1, int(float(pa->n1300) * g_clock.dt / kTMeterFill + 0.5f)));
        SetActorPower(pa, n);
        if (n >= pa->n1300 || g_clock.t - it->second > kTMeterFill * 2.0f) s_mpidTFill.erase(it);
        return;
    }
    if (!FMeterRechargerReady(id) || NActorPower(pa) > 0) return;
    s_mpidTFill[id] = g_clock.t;
    pa->g1304 = g_clock.t;   // (the meter's flash, as a spend shows it)
    s_mpidTUsed[id] = g_clock.t;
    PsndStartSound(0x29);    // (ThiefNet's purchase sound)
    if (FPrint()) std::printf("gadget: Meter Recharger %d refills the meter t %.2f\n", id, g_clock.t);
}

// ---- the table: an item's handlers (null: none)
struct PCPOWERUP
{
    int id;
    bool (*pfnCanDo)(ACTOR* pa, int id);
    void (*pfnStart)(ACTOR* pa, int id);
    void (*pfnEnd)(ACTOR* pa, int id);
    void (*pfnHeld)(ACTOR* pa, int id);
};
const PCPOWERUP s_apup[] = {
    {-1, nullptr, nullptr, nullptr, nullptr},   // (none on a button yet: Meter Recharger is passive, below)
};

const PCPOWERUP* PpupOf(int id)
{
    for (const PCPOWERUP& p : s_apup)
        if (p.id == id) return &p;
    return nullptr;
}
}  // namespace

bool FPcPowerup(int id) { return PpupOf(id) != nullptr; }

bool FPcPowerupCanDo(ACTOR* pa, int id)
{
    const PCPOWERUP* p = PpupOf(id);
    if (!p || !p->pfnStart) return false;
    return !p->pfnCanDo || p->pfnCanDo(pa, id);
}

void StartPcPowerup(ACTOR* pa, int id)
{
    const PCPOWERUP* p = PpupOf(id);
    if (FPrint()) std::printf("gadget: the PC powerup %d (%s) starts\n", id, SzPcItemName(id));
    if (p && p->pfnStart) p->pfnStart(pa, id);
}

void EndPcPowerup(ACTOR* pa, int id)
{
    const PCPOWERUP* p = PpupOf(id);
    if (p && p->pfnEnd) p->pfnEnd(pa, id);
}

void UpdatePcPowerupHeld(ACTOR* pa, int id)
{
    const PCPOWERUP* p = PpupOf(id);
    if (p && p->pfnHeld) p->pfnHeld(pa, id);
}

int HitGuardsAround(ALO* paloPlayer, const glm::vec3& pos, float r, float zRange, unsigned grfFilter, int nState, float vPush, float vUp)
{
    if (!g_psw || !paloPlayer) return 0;
    int c = 0;
    for (CID cid = 0x38; cid <= 0x43; ++cid)
        for (LO* plo : std::vector<LO*>(g_psw->mpcidaplo[cid]))
        {
            if (!plo->fInWorld || !FCidDerivedFrom(plo->Cid(), 0x30) || !PguardFromLo(plo) || FLoInParentChain(plo, paloPlayer)) continue;
            ALO* pguard = static_cast<ALO*>(plo);
            const glm::vec3 posG = PosAloCenter(pguard);
            const glm::vec2 d(posG - pos);
            if (glm::length(d) > r || std::fabs(posG.z - pos.z) > zRange) continue;
            const glm::vec2 dir = glm::length(d) > 1.0f ? glm::normalize(d) : glm::vec2(1.0f, 0.0f);
            CTMSG ctm;
            ctm.ploSender = paloPlayer;
            ctm.ploReceiver = pguard;
            ctm.grfFilter = grfFilter;
            ctm.nState = nState;
            ctm.pos = posG;
            ctm.vec = glm::vec3(dir * vPush, vUp);
            if (PaloAcceptContact(&ctm)) ++c;
            if (FPrint()) std::printf("gadget: PC hit on guard %d (filter 0x%X, %d)\n", plo->oid, grfFilter, nState);
        }
    return c;
}

namespace
{
// Sly 4's Mega Charge Attack: the charged strike (Sly's state 5, substates 8 / 9) hits every guard within 280 once,
// as a strike 3 deep, knocked back
float s_tMegaCharge = -1.0f;   // (the strike it was for: its substate's start)
void UpdateMegaCharge(SLY* p)
{
    if (p->state != 5 || (p->substate != 8 && p->substate != 9) || !FPowerupOwned(kItemMegaChargeAttack)) return;
    if (s_tMegaCharge == p->tSubstate) return;
    if (g_clock.t - p->tSubstate < 0.12f) return;   // (as the swing comes round)
    s_tMegaCharge = p->tSubstate;
    HitGuardsAround(p->palo, PosAloCenter(p->palo), 280.0f, 220.0f, 0x800u, -3, 900.0f, 350.0f);
}

// Sly 4's Scorching Hover Pack: while hovering (the jump's kind 17), the guards below (within 220 across, 700 down)
// are burnt, each once every 1.2 s, as a strike 1 deep
std::unordered_map<const LO*, float> s_mpguardTScorch;
void UpdateScorchingHover(ACTOR* pa)
{
    if (pa->jump.kind != 17 || !FPowerupOwned(kItemScorchingHoverPack) || !g_psw) return;
    const glm::vec3 pos = PosAloCenter(pa->palo);
    for (CID cid = 0x38; cid <= 0x43; ++cid)
        for (LO* plo : std::vector<LO*>(g_psw->mpcidaplo[cid]))
        {
            if (!plo->fInWorld || !FCidDerivedFrom(plo->Cid(), 0x30) || !PguardFromLo(plo)) continue;
            const glm::vec3 posG = PosAloCenter(static_cast<ALO*>(plo));
            if (glm::length(glm::vec2(posG - pos)) > 220.0f || posG.z > pos.z || pos.z - posG.z > 700.0f) continue;
            float& t = s_mpguardTScorch[plo];
            if (t != 0.0f && g_clock.t >= t && g_clock.t - t < 1.2f) continue;
            t = g_clock.t;
            CTMSG ctm;
            ctm.ploSender = pa->palo;
            ctm.ploReceiver = plo;
            ctm.grfFilter = 0x800u;
            ctm.nState = -1;
            ctm.pos = posG;
            ctm.vec = glm::vec3(0.0f, 0.0f, 200.0f);
            PaloAcceptContact(&ctm);
            if (FPrint()) std::printf("gadget: Scorching Hover Pack burns guard %d\n", plo->oid);
        }
}
}  // namespace

void UpdatePcPowerupsPassive(ACTOR* pa)
{
    if (!pa || !pa->palo) return;
    // Meter Recharger: the character's own
    const int id = FIsLoDerivedFrom(pa->palo, 0x35) ? kItemMeterRechargerSly : FIsLoDerivedFrom(pa->palo, 0x37) ? kItemMeterRechargerBentley
                 : FIsLoDerivedFrom(pa->palo, 0x36) ? kItemMeterRechargerMurray : -1;
    if (id != -1) UpdateMeterRecharger(pa, id);
    if (SLY* psly = dynamic_cast<SLY*>(pa)) UpdateMegaCharge(psly);
    if (FIsLoDerivedFrom(pa->palo, 0x37)) UpdateScorchingHover(pa);
}
