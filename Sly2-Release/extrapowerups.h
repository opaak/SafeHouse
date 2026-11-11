#pragma once

// PC addition, not in the original game: the extra powerups, Sly 3's and Sly 4's brought to Sly 2's three. Each is a
// PC item (pcprogress.h: its id after the original's 0x2B, its character, cost, episode, name, description, use and
// power), sold in ThiefNet for coins (shop.cpp's Buy page, after the original's) and, unless passive, put on L1 / L2 /
// R2 from the gadget menu's purchased page (shop.cpp: Select in play, then square; the original's grid has no room
// for them). The button is kept with the slot's PC progress, never in the PS2 record (pcprogress.h IdPcSlot).
//
// A new one: its item in pcprogress.cpp's table (an id, a key for the file), then its handlers here (s_apup in
// extrapowerups.cpp). A passive one needs no handlers: the game's code asks FPowerupOwned(its id) where it acts.
//
// The handlers are the original's per-character action slots for them (gadget.cpp): vt slot 99 (may it start now),
// 100 (it starts), 102 (a held one ends) and 101 (a held one, each frame while held). The power (pcprogress.h) is
// checked before the start as the original's are; the handler spends it (SpendActorPower) as the original's do.

struct ACTOR;
#include <glm/glm.hpp>

bool FPcPowerup(int id);                    // handled here
bool FPcPowerupCanDo(ACTOR* pa, int id);    // vt slot 99
void StartPcPowerup(ACTOR* pa, int id);     // vt slot 100
void EndPcPowerup(ACTOR* pa, int id);       // vt slot 102 (held ones)
void UpdatePcPowerupHeld(ACTOR* pa, int id);   // vt slot 101 (held ones, each frame)
// The passive ones that act on their own (each frame for each player, gadget.cpp UpdateActorAbilities): Meter
// Recharger (the meter run empty fills again at once, then 60 s before it can again)
void UpdatePcPowerupsPassive(ACTOR* pa);

// A hit from the player on every guard within r of pos (not z apart more than zRange), as a guard's own contact would
// give it (so.cpp PaloAcceptContact: the guard's hit reaction, damage and knockout): the contact kind grfFilter, the
// amount nState (negative), pushed outward from pos by vPush (and up by vUp). Returns the guards taken.
struct ALO;
int HitGuardsAround(ALO* paloPlayer, const glm::vec3& pos, float r, float zRange, unsigned grfFilter, int nState, float vPush, float vUp);
