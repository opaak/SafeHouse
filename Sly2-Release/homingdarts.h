#pragma once
// (PC) Sly 4's Homing Darts (pcprogress.h kItemHomingDarts) for Bentley: a lock-on and darts that steer.
// - While Bentley can fire (gang.cpp UpdateBentleyState2), the guard nearest the crosshair (within a third of the
//   picture's half, 6000 away, in sight) is locked: brackets close in on it (DrawHomingLockHud).
// - A dart fired with a lock is aimed at it (FireBentleyDart) and steered toward its centre in flight
//   (projectile.cpp UpdateAccmissAccel), turning at most kRadTurn a second, until it passes it, hits or ends.
#include <glm/glm.hpp>

struct BENTLEY;
struct LO;
struct ALO;

void UpdateHomingLock(BENTLEY* p);                 // each step Bentley could fire
LO* PloHomingLock();                               // the locked guard (null: none)
void NoteHomingDart(ALO* pdart, LO* ptarget);      // a dart fired at the lock
void SteerHomingDart(ALO* pdart, glm::vec3* pdir); // its direction turned toward its target (no target: unchanged)
void DrawHomingLockHud(int width, int height);     // game.cpp, after the HUD
#include <vector>
std::vector<ALO*> HomingDartsInFlight();           // (skillfx.cpp: their trails)
