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

bool FPcItem(int id);
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

// The progress itself: cleared (a new game, the title), read with a slot's game, written with it. pathSlot: the save
// slot's own file (SAVESLOT::path); the PC progress is pathSlot + ".pc".
void ResetPcProgress();
void LoadPcProgress(const std::string& pathSlot);
bool FSavePcProgress(const std::string& pathSlot);
