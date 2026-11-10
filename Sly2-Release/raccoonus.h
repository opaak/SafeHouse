#pragma once

// PC addition, not in the original game: the ancestors' techniques (after Sly 4: leapdragon.h, ninjamoves.h) are
// learned from the Thievius Raccoonus, as Sly 1's moves were.
//
//   - A page per technique, each in one episode's vault (the safe the episode's 30 clue bottles open): the page is
//     found once that vault is open, read from the save's own "fVault" flags (vault.cpp), so saves whose vaults are
//     already open have their pages. Opening the vault shows a message.
//   - The pages are in ThiefNet's third page, "Thievius Raccoonus" (shop.cpp: Square / the stick right from Sell):
//     found ones to train, missing ones with where to find them. A found page's training (confirm on it) closes
//     ThiefNet and gives Sly the technique for a challenge against the clock in the hub (the HUD's panel shows the
//     goal, the progress and the time left); done in time, the technique is learned (kept with the slot:
//     pcprogress.h, the item owned); failed (out of time, or for Silent Sprint the guards alerted), it can be tried
//     again from ThiefNet. The challenges measure what Sly does anywhere, not a set course:
//       Rail Sprint: 1500 along ropes and rails with R1 held, in 60 s
//       Climbing Cobra: 1000 up or down poles and pipes with R1 held, in 45 s
//       Leaping Dragon: three leaps landed, in 90 s
//       Silent Sprint: 15 s of sprinting within 900 of a guard, in 120 s, without the guards alerted
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
bool FConfirmRaccoonusPage();   // (true: a training started, ThiefNet closed)
std::string StrRaccoonusDescription();

// The training (game.cpp each frame, after the step): its progress, its end. The technique of an item in training
// works as if learned (gadget.cpp FPowerupOwned).
void UpdateRaccoonusTraining();
bool FTrainingItem(int item);
// leapdragon.cpp: a leap landed on its target spire
void NoteLeapLanded();

// Drawn over the HUD (game.cpp): the page's list while ThiefNet shows it; the training's panel; the messages.
void DrawRaccoonusHud(int width, int height);
