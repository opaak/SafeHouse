#pragma once

// PC additions' progress, not in the original's save (its 0x5408-byte record is laid out by the game's own progress
// trie, which has no room for new items): a small text file beside each save slot's file, "<slot file>.pc", with one
// "key=value" line per entry. It is read when a slot's game starts (frontend.cpp StartGameFromSave), cleared for a new
// game and at the title, and written whenever the slot is (FWriteSaveSlot). A slot without one owns none of them, so
// older saves and PS2 saves load as before, and the PS2 record itself is never changed.
//
// The PC items: ThiefNet items after the original's 0x2B (ids 0 .. 0x2A, the table at 0x2BCD00). They are sold in
// ThiefNet beside the original's (shop.cpp) and are owned when bought; they take no gadget slot (the shop's own
// passive moves do the same, the item table's use -1).

#include <string>

// Leaping Dragon (leapdragon.h), after Sly 4's Rioichi Cooper: Sly's, on sale from Episode 2.
constexpr int kItemLeapingDragon = 0x2B;
// After Sly 4 too (ninjamoves.h): Sly's sprint (R1, the original's noisy state 1) silent, with shadow afterimages
// (Rioichi's Silent Sprint); faster along ropes and rails with R1 held (ThiefNet's Rail Sprint); faster up and down
// poles and pipes with R1 held (Salim's Climbing Cobra).
constexpr int kItemSilentSprint = 0x2C;
constexpr int kItemRailSprint = 0x2D;
constexpr int kItemClimbingCobra = 0x2E;
// The extra powerups (extrapowerups.h: Sly 3's and Sly 4's, sold in ThiefNet for coins and put on L1 / L2 / R2 like
// the original's). The first, the pipeline's test: Sly 4's Meter Recharger, one for each of the three.
constexpr int kItemMeterRechargerSly = 0x2F;
constexpr int kItemMeterRechargerBentley = 0x30;
constexpr int kItemMeterRechargerMurray = 0x31;
// Sly 4's passive upgrades (use -1: always on once bought; each acts where the original's code asks FPowerupOwned)
constexpr int kItemFlashBomb = 0x32;            // Sly: the Smoke Bomb stuns the guards near it (gadget.cpp)
constexpr int kItemRunWhileCharging = 0x33;     // Sly: walk while winding up the charged swing (sly.cpp)
constexpr int kItemMovingPickpocket = 0x34;     // Sly: walk while picking a pocket (sly.cpp)
constexpr int kItemHomingDarts = 0x35;          // Bentley: the darts aim at the guard nearest the crosshair (gang.cpp)
constexpr int kItemAdrenalineBurstPlus = 0x36;  // Bentley: Adrenaline Burst 1.4 times as fast (gang.cpp)
constexpr int kItemLightAsAFeather = 0x37;      // Murray: run with R1 while carrying, faster (murraycarry.cpp)
constexpr int kItemRocketBoots = 0x38;          // Sly (Sly 3's): the Stealth Slide keeps its speed, half the power (sly.cpp)
constexpr int kItemBombBlast2 = 0x39;           // Bentley: his bombs' blast 1.5 times as wide (carry.cpp BlastCarry)
constexpr int kItemMegaStomp = 0x3A;            // Murray: the Thunder Flop's landing a shockwave round him (gang.cpp)
constexpr int kItemMegaChargeAttack = 0x3B;     // Sly: the charged strike hits every guard round him (extrapowerups.cpp)
constexpr int kItemScorchingHoverPack = 0x3C;   // Bentley: the Hover Pack's jet burns the guards below (extrapowerups.cpp)

bool FPcItem(int id);
// the PC items in ThiefNet's order (i 0 .. CPcItems() - 1)
int CPcItems();
int IdPcItem(int i);
bool FPcItemOwned(int id);
void SetPcItemOwned(int id, bool fOwned);

// ThiefNet's data for a PC item (shop.cpp; the original's are read from the table 0x2BCD00): the character (7 Sly,
// 8 Murray, 9 Bentley), the cost in coins, the episode it goes on sale in (the progress "episodeBest"), its name and
// description (English: the game's own texts are on the disc in its languages).
int NPcItemKind(int id);
int NPcItemCost(int id);
int NPcItemEpisode(int id);
const char* SzPcItemName(int id);
const char* SzPcItemDesc(int id);
// How a PC item is used (gadget.cpp, as the original's table's +0): -1 passive (no button: the techniques, the passive
// powerups), 1 held (started on the press, ended on the release), 2 at once (started and done on the press); the
// gadget power it costs (the table's +0xC); sold in ThiefNet for coins (the extra powerups) or not (the techniques:
// learned from the Thievius Raccoonus, raccoonus.h).
int NPcItemUse(int id);
int NPcItemPower(int id);
bool FPcItemSold(int id);
// its icon's key (techicons.h: built in, or the Mods folder's Icons/<key>.png); null: none
const char* SzPcItemIcon(int id);
// A PC item on a button: kept here, not in the original's record (its "<character> apukCur <j>" holds -1 there, so a
// save stays a PS2 one): ichar 0 Sly, 1 Bentley, 2 Murray; j 0 L1, 1 L2, 2 R2; -1 none.
int IdPcSlot(int ichar, int j);
void SetPcSlot(int ichar, int j, int id);

// The progress itself: cleared (a new game, the title), read with a slot's game, written with it. pathSlot: the save
// slot's own file (SAVESLOT::path); the PC progress is pathSlot + ".pc".
void ResetPcProgress();
void LoadPcProgress(const std::string& pathSlot);
bool FSavePcProgress(const std::string& pathSlot);
