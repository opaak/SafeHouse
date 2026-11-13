#pragma once

// PC modding: props, models of your own placed in a world (Mods/Props). A model is a glTF file in Mods/Props/Models
// (made in Blender, its origin where it stands; or exported from the game, which is stood on its base); a world's
// props are the lines of Mods/Props/w<world>.txt:
//     <model file> <x> <y> <z> <turn> [scale <s>] [tilt <pitch> <roll>] [after <episode 1..8>] [locked]
// (the game's units, centimetres, Z up; turn about Z in degrees; "after 3": shown once episode 3 is complete).
// Each is drawn as one of the frame's extra draws (render.h), lit by the level's light where it stands (the nearest
// level vertices' colours, shaded by its own normals): cel shading, outlines and the ambient occlusion as the level's.
// Not solid (Sly passes through) and not in the level's shadow maps. The debug menu's Display > Graphics > Props places,
// turns, scales, raises and saves them (render.h FCrosshairHit: the point under the crosshair).

#include <glm/glm.hpp>

#include <string>
#include <vector>

struct SCENE;
struct EXTRADRAW;
struct CAMERA;

// ---- the world's views (Mods/Props/w<world>.txt lines "view <name> <x> <y> <z> <yaw> <pitch> [fov]"): the safe house's
// "trophies" view is a stop of its own after ThiefNet and the Raccoonus' (po.cpp), the camera eased to it and back
#define SZ_TROPHY_VIEW "trophies"
bool FPropViewSet(const char* name);
void SetPropViewHere(const char* name);     // from the camera drawn last (the free camera's: aim it with that)
void ClearPropView(const char* name);
// game.cpp, each frame after the game's camera (and the free camera) are set: the trophy stop's view eased in and
// out (not over the free camera); the camera drawn kept for SetPropViewHere
void ApplySafeHouseView(CAMERA& cam, bool fFreeCam);

// the frame's props (render.cpp's extra draws): loaded for the world, made for the GPU when first drawn
void CollectPropDraws(const SCENE& scene, std::vector<EXTRADRAW>& aextra);

// ---- the placement tool (debugmenu.cpp)
int CPropModels();                          // Mods/Props/Models' files (read again by RescanPropModels)
void RescanPropModels();
std::string StrPropModel(int imodel);
int CProps();                               // this world's props
int IpropSelected();                        // -1 none
void SelectProp(int iprop);
void SelectPropNearest(const glm::vec3& pos);
std::string StrPropSelected();              // a line for the menu: its model, place, turn, scale, condition
bool FPlaceProp(int imodel, const glm::vec3& pos, float turnDeg);   // a new prop, selected
void MovePropSelected(const glm::vec3& pos);
void TurnPropSelected(float dDeg);
void TiltPropSelected(float dPitch, float dRoll);   // forward / back (about its X), sideways (about its Y), after its turn
void StraightenPropSelected();              // no tilt
void ScalePropSelected(float r);
void RaisePropSelected(float dz);
void CyclePropSelectedAfter();              // shown: always, after episode 1 .. 8
void DeletePropSelected();
// the snaps (the level's solid surfaces, not the characters'): straight down onto the surface below; onto the nearest
// surface within 5 m (fStand: its up axis along the surface's normal, a picture on a wall, a box on a slope)
bool FDropPropSelected(std::string* pstr);
bool FStickPropSelected(bool fStand, std::string* pstr);
// locked: the tool's edits (moving, turning, the snaps, deleting) leave it as it is ("locked" in the file)
bool FPropSelectedLocked();
void ToggleLockPropSelected();
bool FSaveProps(std::string* pstr);         // Mods/Props/w<world>.txt
