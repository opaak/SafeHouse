// See extrapowerups.h.

#include "extrapowerups.h"

#include "actor.h"
#include "brx.h"
#include "clock.h"
#include "gadget.h"
#include "pcprogress.h"
#include "sound.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

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
// passive (UpdatePcPowerupsPassive): the meter run empty fills again at once, if ready
void UpdateMeterRecharger(ACTOR* pa, int id)
{
    if (!FPowerupOwned(id) || !FMeterRechargerReady(id) || pa->n1300 <= 0 || NActorPower(pa) > 0) return;
    SetActorPower(pa, pa->n1300);
    pa->g1304 = g_clock.t;   // (the meter's flash, as a spend shows it)
    s_mpidTUsed[id] = g_clock.t;
    PsndStartSound(0x29);    // (ThiefNet's purchase sound)
    if (FPrint()) std::printf("gadget: Meter Recharger %d refilled the meter t %.2f\n", id, g_clock.t);
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

void UpdatePcPowerupsPassive(ACTOR* pa)
{
    if (!pa || !pa->palo) return;
    // Meter Recharger: the character's own
    const int id = FIsLoDerivedFrom(pa->palo, 0x35) ? kItemMeterRechargerSly : FIsLoDerivedFrom(pa->palo, 0x37) ? kItemMeterRechargerBentley
                 : FIsLoDerivedFrom(pa->palo, 0x36) ? kItemMeterRechargerMurray : -1;
    if (id != -1) UpdateMeterRecharger(pa, id);
}
