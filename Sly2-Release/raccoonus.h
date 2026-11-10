#pragma once

// PC addition, not in the original game: the ancestors' techniques (after Sly 4: leapdragon.h, ninjamoves.h) are
// learned from the Thievius Raccoonus, as Sly 1's moves were.
//
//   - A page per technique, each in one episode's vault (the safe the episode's 30 clue bottles open): the page is
//     found once that vault is open, read from the save's own "fVault" flags (vault.cpp), so saves whose vaults are
//     already open have their pages. Opening the vault shows a message.
//   - The pages are in ThiefNet's third page, "Thievius Raccoonus" (shop.cpp: Square / the stick right from Sell):
//     found ones to study, missing ones with where to find them. Studying a page learns its technique (kept with
//     the slot: pcprogress.h, the item owned). [Phase 1: studying is choosing it; the training challenges follow.]
//   - The techniques are no longer sold for coins.

#include <string>

struct RACPAGE
{
    int item;                 // pcprogress.h's item (owned: learned)
    int iep;                  // the episode whose vault holds it (WREC::slot: 1 Paris .. 8)
    const char* szAncestor;
    const char* szTechnique;
    const char* szHow;        // how to use it (the game's texts' "&2O&." style button icons)
};
int CRaccoonusPages();
const RACPAGE& RacPage(int i);
bool FRaccoonusPageFound(int i);    // its episode's vault is open
bool FRaccoonusPageLearned(int i);

// vault.cpp: the vault of the current world was just opened (its page's message, if it holds one)
void NoteVaultOpened();

// ThiefNet's Thievius Raccoonus page (shop.cpp): its selection, a step up / down, the confirm (true: something
// happened), the description line for the shop's text box
void ResetRaccoonusPage();
void StepRaccoonusPage(int d);
bool FConfirmRaccoonusPage();
std::string StrRaccoonusDescription();

// Drawn over the HUD (game.cpp): the page's list while ThiefNet shows it; the page-found / learned message.
void DrawRaccoonusHud(int width, int height);
