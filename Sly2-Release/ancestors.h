#pragma once
// (PC) The Thievius Raccoonus' later pages (pcprogress.h, raccoonus.h: episodes 5 to 8), Sly's, each off until learned
// (or while in its training):
//
//   - Knight's Leap (Sir Galleth Cooper's): the double jump with R1 held (as from a sprint) leaps about twice as high,
//     a little further (jump.cpp, right after the double jump starts). Not the Mega Jump: no gadget, no power.
//   - Treasure Sense (Bob Cooper's): the clue bottles still to find within kRSense of Sly glow through walls (a marker
//     over the picture, brighter nearer).
//   - Shadow Cloak (Otto van Cooper's): standing still with R1 held (0.4 s) Sly melts into the shadows: the guards do
//     not see him (detect.cpp NDetectState: as the original's unseen flag), until he moves or lets go. A touch still
//     gives him away.
//   - Coin Magnet (Slytunkhamen Cooper II's): coins and clue bottles within kRMagnet of Sly are his (pickup.cpp
//     FPickupTouched), a gold streak flying to him.
// The main Sly only, as the current player.
#include <glm/glm.hpp>

struct ACTOR;
struct ALO;
struct LO;

void ApplyKnightsLeap(ACTOR* pa);                // jump.cpp: the double jump just started
bool FShadowCloaked(const LO* pso);              // detect.cpp: unseen
float UShadowCloak(ALO** ppalo);                 // its look (skillfx.cpp): 0 none .. 1 full, and whose
bool FCoinMagnetTakes(LO* ppickup, LO* pplayer); // pickup.cpp: taken from afar
void UpdateAncestorTechniques();                 // game.cpp, once a game step
void DrawAncestorHud(int width, int height);     // game.cpp, over the 3D picture: Treasure Sense's markers
