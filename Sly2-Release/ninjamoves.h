#pragma once

// PC additions, not in the original game: three ThiefNet items of Sly's after Sly 4 (pcprogress.h), each off until
// bought.
//
//   - Silent Sprint (Rioichi Cooper's): the sprint (R1 held while running, Sly's state 1, which the original counts as
//     noisy: sly.cpp NActorNoisyState) makes no noise for the guards, and Sly leaves shadow afterimages: every 0.05 s of
//     sprinting a snapshot of his pose, drawn for 0.3 s after as a dark translucent silhouette fading out
//     (render.h RenderGhostPoses: over the 3D picture, behind what is in front of it, each silhouette once per pixel).
//   - Rail Sprint (ThiefNet's in Sly 4): R1 held walking a rope or rail (the interaction kind 3) runs it 1.75 times as
//     fast.
//   - Climbing Cobra (Salim al-Kupar's): R1 held on a pole or pipe (Sly's state 8) climbs it 1.8 times as fast.
//
// The main Sly only, as the current player.

#include <glm/glm.hpp>

struct ACTOR;
struct PADCTL;
struct SCENE;
struct CAMERA;

// The original's noise of the sprint taken away (sly.cpp NActorNoisyState's state 1)
bool FSilentSprint(ACTOR* pa);
// The speed factors (1: as the original): the rail walk's gait velocity (interact.cpp), the climb's (climb.cpp)
float RRailSprint(ACTOR* pa, const PADCTL* pc);
float RClimbCobra(ACTOR* pa, const PADCTL* pc);

// The afterimages (game.cpp): their poses taken from the scene as placed for this frame, before RenderScene; drawn
// after it (before the image passes and the HUD).
void UpdateSprintGhosts(const SCENE& scene);
void DrawSprintGhosts(const SCENE& scene, const CAMERA& cam);
