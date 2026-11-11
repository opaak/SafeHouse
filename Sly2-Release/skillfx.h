#pragma once
// (PC) The look of the extra powerups (pcprogress.h): light effects in the world, drawn over the 3D picture and under
// the HUD (rings on the ground, puffs, flames, sparks, a flash), afterimages through render.h RenderGhostPoses, and the
// original's camera jolt (JoltCm). Only for the current player's own moves; nothing changes how a move plays.
//
//   Mega Charge Attack   a pale ring out from Sly's feet, dust, a jolt
//   Mega Stomp           two rings out to the stomp's reach, a ring of dust, a jolt
//   Bomb Blast v2.0      an orange ring to the wider blast's edge, sparks
//   Flash Bomb           a white flash over the picture (weaker when it is out of sight), a fast white ring
//   Meter Recharger      blue rings rising round the character, sparkles
//   Homing Darts         an orange trail behind each steered dart
//   Scorching Hover Pack the pack's own jet drawn red while hovering, a burst on each guard it burns
//   Rocket Boots         sparks and a glow from Sly's feet while he slides
//   Adrenaline Burst Plus green afterimages behind Bentley
//   Light as a Feather   soft puffs of dust under Murray's run
#include <glm/glm.hpp>

struct ALO;
struct SCENE;
struct CAMERA;

enum SKILLFXK { SKILLFXK_MegaCharge, SKILLFXK_MegaStomp, SKILLFXK_BombBlast };
// pos: the blast, or the mover's place (paloOn: the ground under it is found)
void SkillFxShockwave(const glm::vec3& pos, float r, SKILLFXK k, ALO* paloOn = nullptr);
void SkillFxFlash(const glm::vec3& pos);
void SkillFxRecharge(ALO* palo);
void SkillFxBurn(const glm::vec3& pos);
// each step the move goes on
void NoteSkillFxHover(ALO* palo);
void NoteSkillFxRocketSlide(ALO* palo);
void NoteSkillFxAdrenaline(ALO* palo);
void NoteSkillFxFeather(ALO* palo);

// scenesw.cpp, as each entry is placed (the pass helpers' threads too): the hovering pack's glow kept to be drawn red,
// hidden from the scene
struct SCENEGLOBSET;
void NoteSkillFxPlaced(ALO* palo, SCENEGLOBSET& sgs);

// game.cpp: once a game step (the particles); the afterimages taken before RenderScene and drawn after it; the rest
// over the 3D picture before the HUD
void UpdateSkillFx();
void UpdateSkillFxGhosts(const SCENE& scene);
void DrawSkillFxGhosts(const SCENE& scene, const CAMERA& cam);
void DrawSkillFx(int width, int height);
