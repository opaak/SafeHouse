#pragma once

// PC addition, not in the original game: Leaping Dragon, after Sly 4's Rioichi Cooper ("he focuses on a target from a
// spire and leaps great distances"). A ThiefNet item of Sly's (pcprogress.h kItemLeapingDragon, from Episode 2);
// owned, it works on any spire:
//
//   - perched on a spire (the interaction kind 4), hold circle: the spire to leap to is chosen and marked on the
//     picture: the one nearest the direction of the left stick (or, the stick at rest, of the camera), within 40
//     degrees of it, 250 to 3200 across, at most 900 above and 2000 below, whose leap's path is clear;
//   - let go of circle: Sly turns to it, jumps off and flies there on a long arc (jump.h FPlanLeapToSpire /
//     StartLeapToSpire: circle's own spire target, state 9), landing perched on it;
//   - cross while holding circle jumps off as usual (the aim dropped); without a target, letting go does nothing.
//
// It costs no gadget power and takes no gadget slot. Sly only (the main Sly, as the current player).

struct ACTOR;
struct LO;
struct PADCTL;

// From the spire's interaction transitions (interact.cpp, kind 4), before cross: the aim while circle is held, the
// leap when it is let go. True when the leap started (the transition is done).
bool FLeapDragonSpire(ACTOR* pa, LO* ploSpire, PADCTL* pc);
// The target's mark over the 3D picture while aiming (game.cpp, after the HUD).
void DrawLeapDragonHud(int width, int height);
